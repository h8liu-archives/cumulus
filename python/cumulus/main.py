# Cumulus: Smart Filesystem Backup to Dumb Servers
#
# Copyright (C) 2012  Google Inc.
# Written by Michael Vrable <mvrable@cs.ucsd.edu>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

"""The Python-based Cumulus script.

This implements maintenance functions and is a wrapper around the C++
cumulus-backup program.
"""

import re
import sys

import cumulus
from cumulus import cmd_util
from cumulus import config

class FakeOptions:
    pass

def prune_backups(backup_config, scheme):
    store = cumulus.LowlevelDataStore(backup_config.get_global("dest"))
    snapshot_re = re.compile(r"^(.*)-(.*)$")
    retention = backup_config.get_retention_for_scheme(scheme)
    expired_snapshots = []
    for snapshot in sorted(store.list_snapshots()):
        m = snapshot_re.match(snapshot)
        if m.group(1) != scheme: continue
        timestamp = m.group(2)
        keep = retention.consider_snapshot(timestamp)
        if not keep:
            expired_snapshots.append(snapshot)
    # The most recent snapshot is never removed.
    if expired_snapshots: expired_snapshots.pop()
    print expired_snapshots

    # TODO: Clean up the expiration part...
    for snapshot in expired_snapshots:
        store.store.delete("snapshot", "snapshot-%s.lbs" % snapshot)

    print "Collecting garbage..."
    options = FakeOptions()
    options.store = backup_config.get_global("dest")
    options.dry_run = False
    cmd_util.options = options
    cmd_util.cmd_garbage_collect([])

def main(argv):
    backup_config = config.CumulusConfig(argv[1])
    for scheme in backup_config.backup_schemes():
        print scheme
        prune_backups(backup_config, scheme)

if __name__ == "__main__":
    main(sys.argv)
