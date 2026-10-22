#!/bin/sh
# TEMPORARY: flush.conf of a slot-99 validator.  make-conf.sh <mainchain gRPC host:port> <anchor bid ECX, 0: none> <addnode...>
grpc=$1; bid=$2; shift 2
cat <<CONF
# TEMPORARY: Flush on eCash betanet slot 99 (Photon's), doc/photon-slot.md. Data in <datadir>/betanet-photon.
chain=ecashbeta
sidechainslot=99
[ecashbeta]
server=1
printtoconsole=0
# RPC: localhost only (38994), cookie auth.
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
# P2P: 38995.
listen=1
port=38995
# The mainchain: an enforcer's (or Backbone's) gRPC alone.
mainchaingrpc=$grpc
# The validator key (one secret key in hex), made on this machine.
validatorkeyfile=/opt/flush-photon/data/validator.keys
# What this node bids per betanet block for its anchors, from the mainchain's gRPC wallet (0: none).
validatoranchorbid=$bid
dbcache=100
maxmempool=50
maxconnections=40
CONF
for n in "$@"; do echo "addnode=$n"; done
