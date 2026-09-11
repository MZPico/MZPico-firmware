#!/bin/sh
# Host-side protocol harness for src/mz_devices/unicard.cpp (see docs/unicard-migration-plan.md)
set -e
cd "$(dirname "$0")"
R=../../src
g++ -std=c++17 -O1 -w -DUNICARD_HOST_SIM -I stubs -I $R -I $R/mz_devices -I $R/byte_source \
    sim.cpp stubs/ff_posix.cpp stubs/posix_dir.cpp $R/mz_devices/unicard.cpp $R/mz_devices/unicard_net.cpp $R/net_relay.cpp $R/byte_source/sharpmz_ascii.c -o /tmp/unicard_sim
/tmp/unicard_sim
