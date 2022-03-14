/*
* TLS Channel - implementation for TLS 1.3
* (C) 2021 Elektrobit Automotive GmbH
*
* Botan is released under the Simplified BSD License (see license.txt)
*/

#include <botan/internal/tls_channel_impl_13.h>

#include <botan/hash.h>
#include <botan/internal/tls_cipher_state.h>
#include <botan/internal/tls_handshake_state.h>
#include <botan/internal/tls_record.h>
#include <botan/internal/tls_seq_numbers.h>
#include <botan/internal/stl_util.h>
#include <botan/tls_messages.h>

namespace {
bool is_closure_alert(const Botan::TLS::Alert& alert)
   {
   return alert.type() == Botan::TLS::Alert::CLOSE_NOTIFY
          || alert.type() == Botan::TLS::Alert::USER_CANCELED;
   }

bool is_error_alert(const Botan::TLS::Alert& alert)
   {
   // In TLS 1.3 all alerts except for closure alerts are considered error alerts.
   // (RFC 8446 6.)
   return !is_closure_alert(alert);
   }
}

namespace Botan::TLS {

Channel_Impl_13::Channel_Impl_13(Callbacks& callbacks,
                                 Session_Manager& session_manager,
                                 Credentials_Manager& credentials_manager,
                                 RandomNumberGenerator& rng,
                                 const Policy& policy,
                                 bool is_server,
                                 size_t) :
   m_side(is_server ? Connection_Side::SERVER : Connection_Side::CLIENT),
   m_callbacks(callbacks),
   m_session_manager(session_manager),
   m_credentials_manager(credentials_manager),
   m_rng(rng),
   m_policy(policy),
   m_record_layer(m_side),
   m_handshake_layer(m_side),
   m_can_read(true),
   m_can_write(true)
   {
   }

Channel_Impl_13::~Channel_Impl_13() = default;

size_t Channel_Impl_13::received_data(const uint8_t input[], size_t input_size)
   {
   BOTAN_STATE_CHECK(!is_downgrading());

   // RFC 8446 6.1
   //    Any data received after a closure alert has been received MUST be ignored.
   if(!m_can_read)
      { return 0; }

   try
      {
      if(expects_downgrade())
         preserve_peer_transcript(input, input_size);

      m_record_layer.copy_data(std::vector(input, input+input_size));

      while(true)
         {
         auto result = m_record_layer.next_record(m_cipher_state.get());

         if(std::holds_alternative<BytesNeeded>(result))
            { return std::get<BytesNeeded>(result); }

         const auto& record = std::get<Record>(result);

         // RFC 8446 5.1
         //   Handshake messages MUST NOT be interleaved with other record types.
         if(record.type != HANDSHAKE && m_handshake_layer.has_pending_data())
            { throw Unexpected_Message("Expected remainder of a handshake message"); }

         if(record.type == HANDSHAKE)
            {
            m_handshake_layer.copy_data(unlock(record.fragment));  // TODO: record fragment should be an ordinary std::vector

            while(auto handshake_msg = m_handshake_layer.next_message(policy(), m_transcript_hash))
               {
               // RFC 8446 5.1
               //    Handshake messages MUST NOT span key changes.  Implementations
               //    MUST verify that all messages immediately preceding a key change
               //    align with a record boundary; if not, then they MUST terminate the
               //    connection with an "unexpected_message" alert.  Because the
               //    ClientHello, EndOfEarlyData, ServerHello, Finished, and KeyUpdate
               //    messages can immediately precede a key change, implementations
               //    MUST send these messages in alignment with a record boundary.
               //
               // Note: Hello_Retry_Request was added to the list below although it cannot immediately precede a key change.
               //       However, there cannot be any further sensible messages in the record after HRR.
               //
               // Note: Server_Hello_12 was deliberately not included in the check below because in TLS 1.2 Server Hello and
               //       other handshake messages can be legally coalesced in a single record.
               //
               if(holds_any_of<Client_Hello_13/*, EndOfEarlyData,*/, Server_Hello_13, Hello_Retry_Request, Finished_13/*, KeyUpdate*/>
                     (handshake_msg.value())
                     && m_handshake_layer.has_pending_data())
                  { throw Unexpected_Message("Unexpected additional handshake message data found in record"); }

               const bool downgrade_requested = std::holds_alternative<Server_Hello_12>(handshake_msg.value());

               process_handshake_msg(std::move(handshake_msg.value()));

               if(downgrade_requested)
                  {
                  // Downgrade to TLS 1.2 was detected. Stop everything we do and await being replaced by a 1.2 implementation.
                  BOTAN_STATE_CHECK(m_downgrade_info);
                  m_downgrade_info->will_downgrade = true;
                  return 0;
                  }
               else
                  {
                  // Downgrade can only happen if the first received message is a Server_Hello_12. This was not the case.
                  m_downgrade_info.reset();
                  }
               }
            }
         else if(record.type == CHANGE_CIPHER_SPEC)
            {
            // RFC 8446 5.
            //    An implementation may receive an unencrypted record of type change_cipher_spec
            //    [...]
            //    at any time after the first ClientHello message has been sent or received
            //    and before the peer's Finished message has been received
            //    TODO: Unexpected_Message otherwise
            //    [...]
            //    and MUST simply drop it without further processing.
            // TODO: Send CCS in response / middlebox compatibility mode to be defined via the policy
            }
         else if(record.type == APPLICATION_DATA)
            {
            BOTAN_ASSERT(record.seq_no.has_value(), "decrypted application traffic had a sequence number");
            callbacks().tls_record_received(record.seq_no.value(), record.fragment.data(), record.fragment.size());
            }
         else if(record.type == ALERT)
            {
            process_alert(record.fragment);
            }
         else
            { throw Unexpected_Message("Unexpected record type " + std::to_string(record.type) + " from counterparty"); }
         }
      }
   catch(TLS_Exception& e)
      {
      send_fatal_alert(e.type());
      throw;
      }
   catch(Invalid_Authentication_Tag&)
      {
      // RFC 8446 5.2
      //    If the decryption fails, the receiver MUST terminate the connection
      //    with a "bad_record_mac" alert.
      send_fatal_alert(Alert::BAD_RECORD_MAC);
      throw;
      }
   catch(Decoding_Error&)
      {
      send_fatal_alert(Alert::DECODE_ERROR);
      throw;
      }
   catch(...)
      {
      send_fatal_alert(Alert::INTERNAL_ERROR);
      throw;
      }
   }

void Channel_Impl_13::send_handshake_message(const Handshake_Message_13_Ref message)
   {
   auto msg = m_handshake_layer.prepare_message(message, m_transcript_hash);

   if(expects_downgrade() && std::holds_alternative<std::reference_wrapper<Client_Hello_13>>(message))
      preserve_client_hello(msg);

   send_record(Record_Type::HANDSHAKE, std::move(msg));
   }

void Channel_Impl_13::send_dummy_change_cipher_spec()
   {
   // RFC 8446 5.
   //    The change_cipher_spec record is used only for compatibility purposes
   //    (see Appendix D.4).
   //
   // The only allowed CCS message content is 0x01, all other CCS records MUST
   // be rejected by TLS 1.3 implementations.
   send_record(Record_Type::CHANGE_CIPHER_SPEC, {0x01});
   }

void Channel_Impl_13::send(const uint8_t buf[], size_t buf_size)
   {
   if(!is_active())
      { throw Invalid_State("Data cannot be sent on inactive TLS connection"); }

   send_record(Record_Type::APPLICATION_DATA, {buf, buf+buf_size});
   }

void Channel_Impl_13::send_alert(const Alert& alert)
   {
   if(alert.is_valid() && m_can_write)
      {
      try
         {
         send_record(Record_Type::ALERT, alert.serialize());
         }
      catch(...) { /* swallow it */ }
      }

   // Note: In TLS 1.3 sending a CLOSE_NOTIFY must not immediately lead to closing the reading end.
   // RFC 8446 6.1
   //    Each party MUST send a "close_notify" alert before closing its write
   //    side of the connection, unless it has already sent some error alert.
   //    This does not have any effect on its read side of the connection.
   if(is_closure_alert(alert))
      {
      m_can_write = false;
      m_cipher_state->clear_write_keys();
      }

   if(is_error_alert(alert))
      { shutdown(); }
   }

bool Channel_Impl_13::is_active() const
   {
   return
      m_cipher_state != nullptr && m_cipher_state->can_encrypt_application_traffic() // handshake done
      && m_can_write;  // close() hasn't been called
   }

SymmetricKey Channel_Impl_13::key_material_export(const std::string& label,
      const std::string& context,
      size_t length) const
   {
   BOTAN_STATE_CHECK(!is_downgrading());
   BOTAN_STATE_CHECK(m_cipher_state != nullptr && m_cipher_state->can_export_keys());
   return m_cipher_state->export_key(label, context, length);
   }

void Channel_Impl_13::send_record(uint8_t record_type, const std::vector<uint8_t>& record)
   {
   BOTAN_STATE_CHECK(!is_downgrading());
   BOTAN_STATE_CHECK(m_can_write);
   const auto to_write = m_record_layer.prepare_records(static_cast<Record_Type>(record_type),
                         record, m_cipher_state.get());
   callbacks().tls_emit_data(to_write.data(), to_write.size());
   }

void Channel_Impl_13::process_alert(const secure_vector<uint8_t>& record)
   {
   Alert alert(record);

   if(is_closure_alert(alert))
      {
      m_can_read = false;
      m_cipher_state->clear_read_keys();
      }

   if(is_error_alert(alert))
      { shutdown(); }

   callbacks().tls_alert(alert);
   }

void Channel_Impl_13::shutdown()
   {
   // RFC 8446 6.2
   //    Upon transmission or receipt of a fatal alert message, both
   //    parties MUST immediately close the connection.
   m_can_read = false;
   m_can_write = false;
   m_cipher_state.reset();
   }

void Channel_Impl_13::expect_downgrade(const Server_Information& server_info)
   {
   Downgrade_Information di
      {
      {},
      {},
      server_info,
      callbacks(),
      session_manager(),
      credentials_manager(),
      rng(),
      policy(),
      false // will_downgrade
      };
   m_downgrade_info = std::make_unique<Downgrade_Information>(std::move(di));
   }

}
