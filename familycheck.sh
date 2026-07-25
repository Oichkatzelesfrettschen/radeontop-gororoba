#!/bin/sh
# Check that the two places with family enums are in good condition.
# The next-best thing to the compiler enforcing it.
#
# grep -w matches the enum as a whole word, so a name renamed into a superstring
# (RS480 to RS480_SOMETHING) reads as missing; a plain substring match accepts
# that rename and reports nothing.  A missing name sets the exit status, so a
# build gate fails on it instead of passing with a message on stdout.

fam=$(grep CHIPSET include/r300_pci_ids.h include/r600_pci_ids.h | cut -d, -f3 | cut -d\) -f1 | sed -e 's@^ *@@' -e '/^$/d' | uniq)

rc=0

for i in $fam; do
	grep -qw "$i" family_str.c || { echo "$i missing from family_str.c"; rc=1; }
	grep -qw "$i" include/radeontop.h || { echo "$i missing from include/radeontop.h"; rc=1; }
done

exit $rc
