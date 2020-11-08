/*
* Number Theory Functions
* (C) 1999-2011,2016,2018,2019 Jack Lloyd
* (C) 2007,2008 Falko Strenzke, FlexSecure GmbH
*
* Botan is released under the Simplified BSD License (see license.txt)
*/

#include <botan/numthry.h>
#include <botan/reducer.h>
#include <botan/internal/monty.h>
#include <botan/internal/divide.h>
#include <botan/rng.h>
#include <botan/internal/ct_utils.h>
#include <botan/internal/mp_core.h>
#include <botan/internal/monty_exp.h>
#include <botan/internal/primality.h>
#include <algorithm>

namespace Botan {

namespace {

void sub_abs(BigInt& z, const BigInt& x, const BigInt& y)
   {
   const size_t x_sw = x.sig_words();
   const size_t y_sw = y.sig_words();
   z.resize(std::max(x_sw, y_sw));

   bigint_sub_abs(z.mutable_data(),
                  x.data(), x_sw,
                  y.data(), y_sw);
   }

}

/*
* Tonelli-Shanks algorithm
*/
BigInt ressol(const BigInt& a, const BigInt& p)
   {
   if(p <= 1 || p.is_even())
      throw Invalid_Argument("ressol: invalid prime");

   if(a == 0)
      return 0;
   else if(a < 0)
      throw Invalid_Argument("ressol: value to solve for must be positive");
   else if(a >= p)
      throw Invalid_Argument("ressol: value to solve for must be less than p");

   if(p == 2)
      return a;

   if(jacobi(a, p) != 1) // not a quadratic residue
      return -BigInt(1);

   if(p % 4 == 3) // The easy case
      {
      return power_mod(a, ((p+1) >> 2), p);
      }

   size_t s = low_zero_bits(p - 1);
   BigInt q = p >> s;

   q -= 1;
   q >>= 1;

   Modular_Reducer mod_p(p);

   BigInt r = power_mod(a, q, p);
   BigInt n = mod_p.multiply(a, mod_p.square(r));
   r = mod_p.multiply(r, a);

   if(n == 1)
      return r;

   // find random non quadratic residue z
   BigInt z = 2;
   while(jacobi(z, p) == 1) // while z quadratic residue
      ++z;

   BigInt c = power_mod(z, (q << 1) + 1, p);

   while(n > 1)
      {
      q = n;

      size_t i = 0;
      while(q != 1)
         {
         q = mod_p.square(q);
         ++i;

         if(i >= s)
            {
            return -BigInt(1);
            }
         }

      c = power_mod(c, BigInt::power_of_2(s-i-1), p);
      r = mod_p.multiply(r, c);
      c = mod_p.square(c);
      n = mod_p.multiply(n, c);
      s = i;
      }

   return r;
   }

/*
* Calculate the Jacobi symbol
*/
int32_t jacobi(const BigInt& a, const BigInt& n)
   {
   if(n.is_even() || n < 2)
      throw Invalid_Argument("jacobi: second argument must be odd and > 1");

   BigInt x = a % n;
   BigInt y = n;
   int32_t J = 1;

   while(y > 1)
      {
      x %= y;
      if(x > y / 2)
         {
         x = y - x;
         if(y % 4 == 3)
            J = -J;
         }
      if(x.is_zero())
         return 0;

      size_t shifts = low_zero_bits(x);
      x >>= shifts;
      if(shifts % 2)
         {
         word y_mod_8 = y % 8;
         if(y_mod_8 == 3 || y_mod_8 == 5)
            J = -J;
         }

      if(x % 4 == 3 && y % 4 == 3)
         J = -J;
      std::swap(x, y);
      }
   return J;
   }

/*
* Multiply-Add Operation
*/
BigInt mul_add(const BigInt& a, const BigInt& b, const BigInt& c)
   {
   if(c.is_negative())
      throw Invalid_Argument("mul_add: Third argument must be > 0");

   BigInt::Sign sign = BigInt::Positive;
   if(a.sign() != b.sign())
      sign = BigInt::Negative;

   const size_t a_sw = a.sig_words();
   const size_t b_sw = b.sig_words();
   const size_t c_sw = c.sig_words();

   BigInt r(sign, std::max(a_sw + b_sw, c_sw) + 1);
   secure_vector<word> workspace(r.size());

   bigint_mul(r.mutable_data(), r.size(),
              a.data(), a.size(), a_sw,
              b.data(), b.size(), b_sw,
              workspace.data(), workspace.size());

   const size_t r_size = std::max(r.sig_words(), c_sw);
   bigint_add2(r.mutable_data(), r_size, c.data(), c_sw);
   return r;
   }

/*
* Square a BigInt
*/
BigInt square(const BigInt& x)
   {
   BigInt z = x;
   secure_vector<word> ws;
   z.square(ws);
   return z;
   }

/*
* Return the number of 0 bits at the end of n
*/
size_t low_zero_bits(const BigInt& n)
   {
   size_t low_zero = 0;

   auto seen_nonempty_word = CT::Mask<word>::cleared();

   for(size_t i = 0; i != n.size(); ++i)
      {
      const word x = n.word_at(i);

      // ctz(0) will return sizeof(word)
      const size_t tz_x = ctz(x);

      // if x > 0 we want to count tz_x in total but not any
      // further words, so set the mask after the addition
      low_zero += seen_nonempty_word.if_not_set_return(tz_x);

      seen_nonempty_word |= CT::Mask<word>::expand(x);
      }

   // if we saw no words with x > 0 then n == 0 and the value we have
   // computed is meaningless. Instead return 0 in that case.
   return seen_nonempty_word.if_set_return(low_zero);
   }

/*
* Calculate the GCD
*/
BigInt gcd(const BigInt& a, const BigInt& b)
   {
   if(a.is_zero() || b.is_zero())
      return 0;
   if(a == 1 || b == 1)
      return 1;

   // See https://gcd.cr.yp.to/safegcd-20190413.pdf fig 1.2

   BigInt f = a;
   BigInt g = b;
   f.const_time_poison();
   g.const_time_poison();

   f.set_sign(BigInt::Positive);
   g.set_sign(BigInt::Positive);

   const size_t common2s = std::min(low_zero_bits(f), low_zero_bits(g));
   CT::unpoison(common2s);

   f >>= common2s;
   g >>= common2s;

   f.ct_cond_swap(f.is_even(), g);

   int32_t delta = 1;

   const size_t loop_cnt = 4 + 3*std::max(f.bits(), g.bits());

   BigInt newg, t;
   for(size_t i = 0; i != loop_cnt; ++i)
      {
      sub_abs(newg, f, g);

      const bool need_swap = (g.is_odd() && delta > 0);

      // if(need_swap) delta *= -1
      delta *= CT::Mask<uint8_t>::expand(need_swap).select(0, 2) - 1;
      f.ct_cond_swap(need_swap, g);
      g.ct_cond_swap(need_swap, newg);

      delta += 1;

      g.ct_cond_add(g.is_odd(), f);
      g >>= 1;
      }

   f <<= common2s;

   f.const_time_unpoison();
   g.const_time_unpoison();

   return f;
   }

/*
* Calculate the LCM
*/
BigInt lcm(const BigInt& a, const BigInt& b)
   {
   return ct_divide(a * b, gcd(a, b));
   }

/*
* Modular Exponentiation
*/
BigInt power_mod(const BigInt& base, const BigInt& exp, const BigInt& mod)
   {
   if(mod.is_negative() || mod == 1)
      {
      return 0;
      }

   if(base.is_zero() || mod.is_zero())
      {
      if(exp.is_zero())
         return 1;
      return 0;
      }

   Modular_Reducer reduce_mod(mod);

   const size_t exp_bits = exp.bits();

   if(mod.is_odd())
      {
      const size_t powm_window = 4;

      auto monty_mod = std::make_shared<Montgomery_Params>(mod, reduce_mod);
      auto powm_base_mod = monty_precompute(monty_mod, reduce_mod.reduce(base), powm_window);
      return monty_execute(*powm_base_mod, exp, exp_bits);
      }

   /*
   Support for even modulus is just a convenience and not considered
   cryptographically important, so this implementation is slow ...
   */
   BigInt accum = 1;
   BigInt g = reduce_mod.reduce(base);
   BigInt t;

   for(size_t i = 0; i != exp_bits; ++i)
      {
      t = reduce_mod.multiply(g, accum);
      g = reduce_mod.square(g);
      accum.ct_cond_assign(exp.get_bit(i), t);
      }
   return accum;
   }


BigInt is_perfect_square(const BigInt& C)
   {
   if(C < 1)
      throw Invalid_Argument("is_perfect_square requires C >= 1");
   if(C == 1)
      return 1;

   const size_t n = C.bits();
   const size_t m = (n + 1) / 2;
   const BigInt B = C + BigInt::power_of_2(m);

   BigInt X = BigInt::power_of_2(m) - 1;
   BigInt X2 = (X*X);

   for(;;)
      {
      X = (X2 + C) / (2*X);
      X2 = (X*X);

      if(X2 < B)
         break;
      }

   if(X2 == C)
      return X;
   else
      return 0;
   }

/*
* Test for primality using Miller-Rabin
*/
bool is_prime(const BigInt& n,
              RandomNumberGenerator& rng,
              size_t prob,
              bool is_random)
   {
   if(n == 2)
      return true;
   if(n <= 1 || n.is_even())
      return false;

   const size_t n_bits = n.bits();

   // Fast path testing for small numbers (<= 65521)
   if(n_bits <= 16)
      {
      const uint16_t num = static_cast<uint16_t>(n.word_at(0));

      return std::binary_search(PRIMES, PRIMES + PRIME_TABLE_SIZE, num);
      }

   Modular_Reducer mod_n(n);

   if(rng.is_seeded())
      {
      const size_t t = miller_rabin_test_iterations(n_bits, prob, is_random);

      if(is_miller_rabin_probable_prime(n, mod_n, rng, t) == false)
         return false;

      if(is_random)
         return true;
      else
         return is_lucas_probable_prime(n, mod_n);
      }
   else
      {
      return is_bailie_psw_probable_prime(n, mod_n);
      }
   }

}
