YETIsense update utilities
=========================

This is a collection of firmware upgrade tools specifically written
for YETIsense based on FreeBSD ideas (kernel and base sets) and tools
(pkg(8) and freebsd-update(8)).

yetisense-update
===============

yetisense-update(8) unifies the update process into a single tool
usable from the command line. Since YETIsense uses FreeBSD's package
manager, but not the native upgrade mechanism, an alternative way
of doing base and kernel updates needed to be introduced.

The process relies on signature verification for all moving parts
(packages and sets) by plugging into pkg(8)'s native verification
mechanisms.

The utility was first introduced in February 2015.  In October 2016,
major FreeBSD version upgrade support was added.  In August 2017,
debug kernel support was added.

yetisense-bootstrap
==================

yetisense-bootstrap(8) is a tool that can completely reinstall a
running system in place for a thorough factory reset or to restore
consistency of all the YETIsense files.  It can also wipe the
configuration directory, but won't do that by default.

It will automatically pick up the latest available version and
build a chain of trust by using current package fingerprints -> CA
root certificates -> HTTPS -> YETIsense package fingerprints.

What it will also do is turn a supported stock FreeBSD release into
an YETIsense installation.  Both UFS and ZFS installations are supported.

The usage is simple, starting with a FreeBSD 14.2-RELEASE image:

    # fetch https://raw.githubusercontent.com/Christopher-YeTI/update/master/src/bootstrap/yetisense-bootstrap.sh.in
    # sh ./yetisense-bootstrap.sh.in -r 25.1

After successful reboot, YETIsense should be up and running.  :)

The utility was first introduced in November 2015.

yetisense-sign, yetisense-verify
==============================

yetisense-sign(8) and yetisense-verify(8) sign and verify arbitrary
files using signature verification methods available by pkg(8),
so that a single key store can be used for packages and sets.

yetisense-verify(8) is based on the existing pkg bootstrap code present
in the FreeBSD base code, but has been improved for multi-repo use.

Both utilities were first introduced in December 2015.

yetisense-fetch
==============

yetisense-fetch(8) creates a watcher process for fetch(1) and passes
all arguments to it.  The watcher then prints progress output to the
actual caller to indicate ongoing download progress.

The utility was first introduced in April 2016.

yetisense-patch
==============

yetisense-patch(8) applies upstream git(1) patches in the order that they
have been given.  This helps to deploy fixes faster without the need
to run manual edits or file downloads since patch(1) tries to keep the
file integrity intact.

The utility was first introduced in May 2016 to enable core and plugins
patching.  In February 2019, a local caching mechanism was added to provide
offline patching capability.  In July 2021 support for patching installer
and update tools was added.

yetisense-code
=============

Deriving from the utility of yetisense-patch(8), its younger sibling
yetisense-code(8) can handle full code repositories using git(1)
in order to fetch or update the full source code on an installed system.

The utility was first introduced in August 2016.

yetisense-revert
===============

In the available scope of the package mirrors, this utility can
revert any package to a previous state of a particular YETIsense
release.

The utility was first introduced in January 2017.
