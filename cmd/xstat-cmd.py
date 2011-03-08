#!/usr/bin/env python

# Copyright (C) 2010 Rob Browning
#
# This code is covered under the terms of the GNU Library General
# Public License as described in the bup LICENSE file.

import errno
import posix1e
import stat
import sys
from bup import metadata
from bup import options
from bup import xstat
from bup.helpers import handle_ctrl_c, saved_errors, add_error, log


def fstimestr(fstime):
    (s, ns) = fstime.secs_nsecs()
    if ns == 0:
        return '%d' % s
    else:
        return '%d.%09d' % (s, ns)


optspec = """
bup pathinfo [OPTION ...] <PATH ...>
--
v,verbose       increase log output (can be used more than once)
q,quiet         don't show progress meter
exclude-fields= exclude comma-separated fields
include-fields= include comma-separated fields (definitive if first)
"""

target_filename = ''
all_fields = frozenset(['path',
                        'mode',
                        'link-target',
                        'rdev',
                        'uid',
                        'gid',
                        'owner',
                        'group',
                        'atime',
                        'mtime',
                        'ctime',
                        'linux-attr',
                        'linux-xattr',
                        'posix1e-acl'])
active_fields = all_fields

handle_ctrl_c()

o = options.Options(optspec)
(opt, flags, remainder) = o.parse(sys.argv[1:])

treat_include_fields_as_definitive = True
for flag, value in flags:
    if flag == '--verbose' or flag == '-v':
        metadata.verbose += 1
    elif flag == '--quiet' or flag == '-q':
        metadata.verbose = 0
    elif flag == '--exclude-fields':
        exclude_fields = frozenset(value.split(','))
        for f in exclude_fields:
            if not f in all_fields:
                o.fatal(f + ' is not a valid field name')
        active_fields = active_fields - exclude_fields
        treat_include_fields_as_definitive = False
    elif flag == '--include-fields':
        include_fields = frozenset(value.split(','))
        for f in include_fields:
            if not f in all_fields:
                o.fatal(f + ' is not a valid field name')
        if treat_include_fields_as_definitive:
            active_fields = include_fields
            treat_include_fields_as_definitive = False
        else:
            active_fields = active_fields | include_fields

for path in remainder:
    try:
        m = metadata.from_path(path, archive_path = path)
    except IOError, e:
        if e.errno == errno.ENOENT:
            add_error(e)
            continue
        else:
            raise
    if 'path' in active_fields:
        print 'path:', m.path
    if 'mode' in active_fields:
        print 'mode:', oct(m.mode)
    if 'link-target' in active_fields and stat.S_ISLNK(m.mode):
        print 'link-target:', m.symlink_target
    if 'rdev' in active_fields:
        print 'rdev:', m.rdev
    if 'uid' in active_fields:
        print 'uid:', m.uid
    if 'gid' in active_fields:
        print 'gid:', m.gid
    if 'owner' in active_fields:
        print 'owner:', m.owner
    if 'group' in active_fields:
        print 'group:', m.group
    if 'atime' in active_fields:
        print 'atime: ' + fstimestr(m.atime)
    if 'mtime' in active_fields:
        print 'mtime: ' + fstimestr(m.mtime)
    if 'ctime' in active_fields:
        print 'ctime: ' + fstimestr(m.ctime)
    if 'linux-attr' in active_fields and m.linux_attr:
        print 'linux-attr:', hex(m.linux_attr)
    if 'linux-xattr' in active_fields and m.linux_xattr:
        for name, value in m.linux_xattr:
            print 'linux-xattr: %s -> %s' % (name, repr(value))
    if 'posix1e-acl' in active_fields and m.posix1e_acl:
        flags = posix1e.TEXT_ABBREVIATE
        if stat.S_ISDIR(m.mode):
            acl = m.posix1e_acl[0]
            default_acl = m.posix1e_acl[2]
            print acl.to_any_text('posix1e-acl: ', '\n', flags)
            print acl.to_any_text('posix1e-acl-default: ', '\n', flags)
        else:
            acl = m.posix1e_acl[0]
            print acl.to_any_text('posix1e-acl: ', '\n', flags)

if saved_errors:
    log('WARNING: %d errors encountered.\n' % len(saved_errors))
    sys.exit(1)
else:
    sys.exit(0)
