#!/bin/sh
# A local Flush playground on regtest with eCash's rules: see flush-playground.py (or --help).
#   contrib/playground/flush-playground.sh start|stop|status|mine N|withdraw-demo|cli <node> <method> ...|reset
exec python3 "$(dirname "$0")/flush-playground.py" "$@"
