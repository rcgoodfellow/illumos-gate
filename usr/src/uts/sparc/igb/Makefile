#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#
#
# Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# uts/sparc/igb/Makefile
#
#	This makefile drives the production of the igb
#	network driver kernel module.
#
#	SPARC architecture dependent
#

#
#	Path to the base of the uts directory tree (usually /usr/src/uts).
#
UTSBASE	= ../..

#
#	Define the module and object file sets.
#
MODULE		= igb
OBJECTS		= $(IGB_OBJS:%=$(OBJS_DIR)/%)
OBJECTS		+= $(E1000API_OBJS:%=$(OBJS_DIR)/%)
ROOTMODULE	= $(ROOT_DRV_DIR)/$(MODULE)
CONF_SRCDIR	= $(UTSBASE)/common/io/igb

#
#	Include common rules.
#
include $(UTSBASE)/sparc/Makefile.sparc


#
# For now, disable these warnings; maintainers should endeavor
# to investigate and remove these for maximum coverage.
# Please do not carry these forward to new Makefiles.
#
CFLAGS          += $(CCVERBOSE)
CERRWARN	+= -_gcc=-Wno-switch
CERRWARN	+= $(CNOWARN_UNINIT)
CERRWARN	+= -_cc=-erroff=E_STATEMENT_NOT_REACHED

#
#	Define targets
#
ALL_TARGET	= $(BINARY) $(SRC_CONFFILE)
INSTALL_TARGET	= $(BINARY) $(ROOTMODULE) $(ROOT_CONFFILE)

#
#	Override defaults
#
INC_PATH	+= -I$(CONF_SRCDIR) -I$(UTSBASE)/common/io/e1000api

#
# Turn on doubleword alignment for 64 bit registers
#
CFLAGS		+= -dalign

#
# Driver depends on MAC
#
LDFLAGS		+= -dy -N misc/mac

#
#	Default build targets.
#
.KEEP_STATE:

def:		$(DEF_DEPS)

all:		$(ALL_DEPS)

clean:		$(CLEAN_DEPS)

clobber:	$(CLOBBER_DEPS)

install:	$(INSTALL_DEPS)

#
#	Include common targets.
#
include $(UTSBASE)/sparc/Makefile.targ

