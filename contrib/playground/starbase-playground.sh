#!/bin/sh
# A local Starbase playground on regtest with eCash's rules: see starbase-playground.py (or --help).
#   contrib/playground/starbase-playground.sh start|stop|status|mine N|withdraw-demo|cli <node> <method> ...|reset
exec python3 "$(dirname "$0")/starbase-playground.py" "$@"
