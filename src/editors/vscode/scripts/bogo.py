#!/usr/bin/env python3

import os
from common import run_cmd, get_concurrency


BORING_REPO = "https://github.com/reneme/boringssl.git"
BORING_BRANCH = "rene/runner-20220322"

BORING_PATH = "build_deps/boringssl"
BOGO_PATH = os.path.join(BORING_PATH, "ssl", "test", "runner")

SHIM_PATH = "./botan_bogo_shim"
SHIM_CONFIG = "src/bogo_shim/config.json"


def main():
    if not os.path.isdir(BORING_PATH):
        # check out our fork of boring ssl
        run_cmd("git clone --depth 1 --branch %s %s %s" %
                (BORING_BRANCH, BORING_REPO, BORING_PATH))

    # make doubly sure we're on the correct branch
    run_cmd("git -C %s checkout %s" % (BORING_PATH, BORING_BRANCH))

    run_cmd("go test -pipe -num-workers %d -shim-path %s -shim-config %s" %
            (get_concurrency(), os.path.abspath(SHIM_PATH), os.path.abspath(SHIM_CONFIG)), BOGO_PATH)


if __name__ == '__main__':
    main()
