/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2022 Oxide Computer Co.
 */

/*
 * Nexus driver for the FCHs ("Fusion Controller Hub") found in EPYC SoCs and
 * potentially (with future expansion) in some client processors and chipsets.
 *
 * This is a fantastically long theory statement for a very simple driver.
 * While the nexus driver interface (NDI) is undocumented, it's not very
 * complicated, and this driver doesn't contain a great deal of code.  Most of
 * what it does could be accomplished by some driver.conf files and pseudonex,
 * in fact.  The reason most of this is here is that it isn't anywhere else and
 * I don't want to lose it: there is a lot of material here that while not
 * absolutely specific to the FCH, is absolutely germane both to how this driver
 * is implemented today (poorly) and to how it really should be implemented.
 * The latter requires a foray into the DDI, PSM, rootnex, and 40 years of
 * history that began 3 ISAs ago.  You can probably skip this and just look at
 * the code if you're trying to add or fix something here; most of this is
 * really about how to fix all that *other* stuff, and the code isn't difficult.
 * As we'll see, with the right infrastructure, this driver would be not merely
 * simple but utterly trivial.  This, then, is a love letter to the future we
 * might hope to create.  Let's start with the basic FCH-specific stuff.
 *
 * ==================
 * FCH Identification
 * ==================
 *
 * There is no known internal means of discovering what kind of FCH is present
 * even if we know the range of addresses it decodes.  Some evidence indicates
 * there's an undocumented e-fuse we could read that contains an identifier, but
 * none of the PPRs mention it and it always reads zero.  So we have to assume
 * the type based on our processor family as reported by the cpuid chiprev
 * mechanism.  While some peripherals (see note below) do have registers we
 * could read to determine their revision, the set of peripheral revisions
 * available to us does not uniquely identify an FCH as several different FCHs
 * incorporate peripherals with the same revisions.  While it will not be
 * terribly difficult to add support for any of several additional FCH models
 * that exist, at present we support only these:
 *
 * FCH		Processor Family
 * -----------------------------
 * Huashan	Rome, Milan
 * Songshan	Genoa
 *
 * The Taishan FCH incorporated into Naples processors poses challenges that
 * others do not, on account of its internal multi-die organisation, though most
 * of these challenges pertain to our children.
 *
 * Note that (nearly) all FCHs are named for mountains, including the well-known
 * Promontory client parts and the misfits, Shang and Shasta.  If the ambiguous
 * name Hengshan has been used for an FCH, we don't know of it.
 *
 * ==================
 * Child Nomenclature
 * ==================
 *
 * Several of the FCH's peripherals are really behind a poorly-documented
 * AXI-to-AHB-to-APB bridge, part of what AMD calls the A-Link/B-Link bridge.
 * Ideally we might be able to give them names like we do with PCI; e.g.,
 * apbXXXX,YYYY.  This would allow us to use aliases and in theory to support
 * these fairly common devices even if on some future platform they're found on
 * some other nexus such as a native APB or AHB nexus.  Unfortunately, while
 * Synopsys/DesignWare seem to have adopted at least a semi-consistent practice
 * of putting a pair of registers at the end of each device's 256-byte region
 * that identify the peripheral and its version, nothing in the AMBA
 * specifications even remotely suggests that discovery and enumeration are part
 * of the standard or that peripherals are expected to provide any means (never
 * mind a specific means) of doing this.  Indeed, the concept of a peripheral
 * having registers at all isn't discussed.  So in the service of hardware we do
 * have, we'll do the simple thing and name nodes after our own drivers.  Maybe
 * someday this can be better.  Individual leaf drivers can and should make use
 * of the identifying information available, if any.
 *
 * Each child has a static definition, and each FCH model has a static
 * definition containing them.  This approach is not terribly different from the
 * concept of passing the kernel a static DeviceTree at boot and having the
 * kernel then set up pinmuxing and other configuration to realise it; however,
 * we're aiming for something more flexible that allows for use of multiple
 * conflicting peripherals and runtime configuration.  Additionally, this is
 * only a small part of the overall system device hierarchy and for the most
 * part there is no reason to expose any of this information outside this
 * implementation.
 *
 * ==========
 * Addressing
 * ==========
 *
 * In multi-socket (and older multi-die) systems there may be multiple FCHs in
 * the data fabric (DF).  The first FCH, attached to the DF via one of the IOMSs
 * (IOMS 3 in Milan, potentially a different one in other implementations), is
 * accessible via the subtractive address space at [0xfec0_0000, 0xffff_ffff].
 * The entire set of peripherals in secondary FCHs is not available to us, nor
 * can peripherals in secondary FCHs generate interrupts.  However, it is
 * possible to access a single 8 KiB region of each secondary FCH if
 * FCH::PM::ALTMMIO{BASE,EN} have been set up.  This region corresponds to the
 * peripherals at [0xfed80_0000, 0xfed8_1fff] on the primary FCH.  This region
 * actually contains many disparate peripherals but their registers always share
 * a page so we can't currently protect leaf drivers from one another.
 *
 * In principle, this nexus should be a child of the IOMS to which it's
 * attached, and that IOMS's driver should have created appropriate "ranges" and
 * other properties prior to our attaching to identify the resources available
 * to us and our children.  Because that doesn't exist, we use the same
 * hackaround used by pci_autoconfig to generate PCI bus nexi and by isa to
 * generate its own node: the fch_enumerate() routine does what the parent we
 * don't have should have done for us.  Although that parent doesn't currently
 * exist, we still rely on other software reserving the resources we need and
 * providing them to us, currently via milan_gen_resource_subsume() which is
 * also analogous to the PCI PRD mechanism but without the intermediate
 * abstraction that would be required to make this driver machine-independent.
 * That software must also ensure that access to those MMIO and legacy IO
 * regions is routed over the DF to the correct IOMS.
 *
 * Each child regspec definition is relative to the FCH's base address or to the
 * base address the FCH would have if it were the primary FCH.  This allows us
 * to use the same address offsets for children of both primary and secondary
 * FCHs and therefore to use the same child definitions.  The simplest way to
 * think about this is that the base address is the address of the register
 * block given by the PPR less 0xfec0_0000 (at least for all the FCHs we
 * currently know about).  These are adjusted to absolute physical addresses
 * during the child initialisation process, so that the "reg" properties in the
 * device tree end up looking very much like they do for PCI devices, without
 * the bus number and attributes found in the first 32 bits.  Most FCH
 * peripherals' registers can be accessed via either SMN or MMIO, but
 * unfortunately there is not a single straightforward way to translate the MMIO
 * address of a peripheral to the SMN address or vice versa.  See the address
 * space map and notes in sys/amdzen/fch.h for some more background.  In future
 * we may wish to provide children access to their registers via SMN access
 * handles instead of MMIO, especially if access to secondary FCH peripherals
 * not included in the tiny alternate MMIO BAR is desired.  Children would not
 * be aware of this, just as they are unaware in principle of the distinction
 * between legacy IO space and MMIO space today.
 *
 * Interrupts
 *
 * Most but not all peripherals we support can generate interrupts.  In order to
 * understand how they are implemented here, a great deal of background is
 * needed.  A few bits of this background can be found in os/intr.c, but the
 * focus there is primarily on what happens once a CPU is interrupted.  This can
 * be read as a companion to that; it really belongs somewhere else, along with
 * most of the interrupt functionality in this driver, as will be discussed.
 * Our focus here is on what needs to happen in order for a CPU to be
 * interrupted when one of our children signals an interrupt.  In the distant
 * past, and on some hardware architectures even still today, this was very
 * simple.  For us it is anything but.  If there were theory statements in
 * io/apix/apix.c or os/ddi_impl.c, or any documentation whatsoever describing
 * their operation, you'd be reading those instead, but there aren't so get
 * comfortable.
 *
 * On very old (pre-8086) and very simple (some microcontrollers today)
 * hardware, the physical microprocessor has some number of physical input pins
 * that allow external devices to generate interrupts.  Usually each pin
 * corresponds to a specific interrupt or vector number; the device asserts the
 * interrupt, the processor saves state and hands control to the software at the
 * location corresponding to that vector.  There is a tremendous amount of
 * confusion in terminology in this area: the identity of the interrupt may be
 * called a vector or an IRQ or an interrupt number or an interrupt line or an
 * interrupt pin or very probably several other names, but the essence of it is
 * that there is an integer that describes both the source of the interrupt and
 * the manner in which it is delivered: the source implies a CPU-visible vector
 * number and, on some architectures, a priority level:
 *
 * +--------------+         INTR 0 +-----------+	Interrupt Vector Table
 * | Peripheral A |--------------->| Processor |-+        +----------------+
 * +--------------+       +------->|  (core)   | | Trap N | Handler N Addr |
 *                        | INTR 1 +-----------+ |        +----------------+
 * +--------------+       |                      |        |       ~~~      |
 * | Peripheral B |-------+                      |        +----------------+
 * +--------------+                              | Trap 1 | Handler B Addr |
 *                                               |        +----------------+
 *                                               | Trap 0 | Handler A Addr |
 *                                               +------->+----------------+
 *
 * What has occurred since then consists of the addition of numerous layers of
 * abstraction as well as mechanical changes needed to accommodate large numbers
 * of devices in switched fabrics as well as multiple processors.  We will skip
 * ahead (ignoring the legacy 8259/A interrupt controller discussed in
 * os/intr.c) to the current world, which retains the IVT, called the IDT on
 * x86, but replaces nearly everything else between it and the peripherals
 * themselves.  Additionally, on many hardware architectures, including ours,
 * many of these peripherals are contained in the same package as the processor
 * core(s).  The FCH and the peripherals it contains used to be (and on Intel
 * platforms, still are) called a southbridge; before that, the peripherals were
 * separate from the southbridge itself, which contained only the glue logic for
 * routing transactions and performing bus arbitration.  Even farther in the
 * past, the southbridge itself would have comprised multiple independent
 * packages, which along with the northbridge were called a chipset once vendors
 * started offering integrated collections of parts to perform these functions
 * together.  Regardless of how these things are packaged, on all AMD platforms
 * since the beginning of the 21st century, this functionality looks more or
 * less like this:
 *
 * Let's talk assumptions about node properties.  This should really be on its
 * way to a committed interface described in the manual, but given how awful
 * it's been historically and in many ways still is (especially on i86pc),
 * perhaps it's for the best that it isn't.  A handful of properties are
 * documented in sysbus(4) and pci(4); at present, both of these man pages are
 * largely obsolete, referring to technologies such as Solaris and PCI-X in the
 * present tense, though some of their limited descriptions of IEEE 1275 style
 * properties remains correct.  The manual also assumes that all x86 systems
 * running illumos use the i86pc kernel, platform drivers, and conventions; this
 * has in general been true historically but our existence renders this
 * assumption inaccurate.  Additionally, there is no sysbus driver and the man
 * page describing it refers to ISA as an "x86 ... system bus" which it
 * certainly is not on any machine supported by now 64-bit-only illumos.
 *
 * In the long run, we might want to replace all of these node properties with
 * private data, perhaps faking up equivalent output for prtconf(8) and similar
 * tools.  Or we might want to use 1275-style properties exclusively and provide
 * more convenience functions for leaf drivers, nexus drivers, and DDI/NDI code
 * to interpret them.  In reality, the way this works today is that some code
 * looks up data in 1275 properties while other code uses private data storage,
 * and quite a lot of code especially here and in rootnex actually uses both:
 * much of what's going on here consists of reading 1275 properties and
 * translating them into various private data structures that are then passed
 * around.  The code that ultimately consumes that may or may not understand the
 * 1275 properties, may or may not get a dev_info_t along with the private data
 * or handle, and may or may not follow the same conventions as the original
 * device driver.  In many cases, there is no good way to know what the data
 * type of a child or parent private data structure even is unless you are the
 * driver that attached it, yet there are many places here in the DDI/NDI and in
 * the rootnex driver that make all kinds of assumptions about both the 1275
 * properties and associated private data.  To the best of my knowledge, this is
 * the first halfway serious attempt to describe what those assumptions are.
 *
 * ==========
 * Interrupts
 * ==========
 *
 * First, a bit of terminology.
 *
 * There are three different terms used in discussing interrupts that in the
 * past were used more or less interchangeably.  Much code still exists that
 * refers to one of these concepts using a different (and conflicting) name.
 *
 * vector/vec: This is an amd64 architectural concept.  Each CPU has 256
 * interrupt vectors, of which the first 16 are reserved for exceptions.
 * Vectors are associated with *delivery* of interrupts to one or more CPUs.
 * Any number of different interrupt sources may be delivered to the same CPU on
 * the same vector.  There is much code that uses this to refer to an IRQ, even
 * though IRQ->(apicid, vector) mapping was made indirect with the introduction
 * of the local and IO APICs over 15 years ago.  The possible set of (apicid,
 * vector) destinations for any given interrupt source depends on the
 * configuration of the APICs, which can be and often is changed dynamically
 * based on the state of CPUs and interrupt balancing policies.  See the big
 * theory statement in os/intr.c for more details about how all this works, as
 * well as the AMD64 architecture manual vol. 2 chapters 8 and 16.
 *
 * IRQ: An IRQ is an OS concept, an implementation detail of the IOAPIC and the
 * PSM code responsible for managing interrupts (apix, on oxide; possibly
 * pcplusmp or uppc on i86pc).  illumos uses IRQ alternately to refer to a
 * global index into the set of IOAPIC virtual wire inputs or to a specific
 * virtual wire input to a specific IOAPIC.  On i86pc, the PIC is also still
 * supported, which is an obsolete technology that mapped interrupt sources onto
 * a fixed set of IRQ numbers that had a fixed 1-1 mapping onto primitive CPUs'
 * vector space.  Today, on most modern x86 implementations, any fixed interrupt
 * source can be mapped onto any virtual wire input on at least one IOAPIC, and
 * every virtual wire input on every IOAPIC can be mapped to any destination.
 * The IRQ itself is a convenience for identifying the hardware mechanism for
 * mapping an interrupt *source* to an interrupt *destination*.
 *
 * interrupt number/inum: With the introduction of the "new" DDI interrupt
 * routines introduced to support MSI-X in 2003-2005, this refers simply to an
 * index into an array of possible interrupts a device can generate.  Each one
 * represents a particular source, which may be an MSI interrupt, an MSI-X
 * interrupt, or a fixed interrupt which for PCI/PCIe may be INTA, INTB, etc.
 * For non-PCI devices, the set of possible interrupt sources depends on the
 * device itself, the machine and processor implementation, and the illumos
 * machine architecture.  On PCs, fixed interrupt sources are for the most part
 * permanently bound by firmware to a specific virtual wire input to a specific
 * IOAPIC (an IRQ number); on the oxide architecture, we are free to associate
 * each source with any mechanism the hardware permits.  As the interrupt number
 * is merely an index, the underlying meaning of the interrupt source has to
 * come from somewhere.  On i86pc, it's an IRQ number that comes from ACPI
 * tables associated with the source device; on oxide, it's a hardware source
 * identifier that can be mapped onto an IRQ by an internal switch.
 *
 * There are a few other less confusing terms we'll encounter:
 *
 * ipl/spl/priority: This is an integer that describes a policy associated with
 * delivery of an interrupt.  The BTS in os/intr.c discusses this in some
 * detail; importantly, the association between ipl and vector is fixed on i86pc
 * when using the uppc/pcplusmp PSMs (for legacy PIC/xAPIC) but this constraint
 * is relaxed when x2APIC hardware is available and thus apix can be used
 * instead.  The oxide architecture requires x2APIC hardware, supports only
 * apix, and always operates in x2APIC mode.  It is possible for a device driver
 * or an operator to request that each of its interrupt sources be delivered at
 * a particular priority via the interrupt-priorities 1275 property, discussed
 * below.
 *
 * PCIe INTx Emulation
 *
 * In the original PCI spec, PCI devices could generate interrupts on one of 4
 * (or sometimes 8) physical pins, lettered A through H.  A complex and probably
 * needlessly confusing swizzling mechanism was defined so that as each end
 * device's interrupt wires were routed through a series of bridges, they would
 * be mapped onto (really, physically connected to) a different interrupt wire
 * on the next upstream bus segment.  This was intended to limit forced IRQ
 * sharing, because each interrupt wire on the bus connected to a host bridge
 * could generate only a single IRQ.  These interrupts, then, were essentially a
 * shared bus not dissimilar to I2C: to assert an interrupt, a device would pull
 * one of its interrupt pins low; if any device did so, the host bridge would
 * interrupt the CPU (later, an IOAPIC) on a vector associated with that IRQ.
 * This is why legacy PCI interrupts are always level-triggered and active-low.
 * Each device supported one or more of the interrupt pins, and software could
 * select which one to use.  It was and is fairly common to support only INTA,
 * relying on the system implementation to limit undesirable sharing; even so,
 * in this era it was common to recommend moving a device from one slot to
 * another to eliminate sharing, as there was often no other way to do so.
 *
 * With the introduction of message-based serial interconnects (i.e., PCIe), the
 * individual interrupt wires were replaced by message-signalled interrupts
 * (MSI and later MSI-X) but an emulation mechanism was introduced for the
 * purpose of allowing downstream devices on the far side of a PCIe-PCI bridge
 * to generate interrupts in a straightforward manner.  It is also possible for
 * PCIe devices to be configured to generate these fixed interrupts, but as they
 * are strictly inferior to native MSI in every way, all illumos drivers have
 * been updated to support the native mechanisms.  Upon arrival at the root
 * complex, the legacy INTx emulation messages are mapped onto a set of internal
 * interrupt sources, one for each of the 8 emulated interrupt wires.  All such
 * messages (subject again to swizzling between their source and the root
 * complex) that arrive at the root complex with a specific emulated wire name
 * share a single interrupt source.  Each source may be mapped onto IOAPIC
 * virtual wire inputs in a hardware-specific manner; as with PCI INTx wires, on
 * PCs these mappings are constructed by firmware prior to boot and are
 * considered fixed; they are communicated to the OS via ACPI or, on machines
 * with *very* old firmware, an Intel MP BIOS data structure.
 *
 * On machines implementing the oxide archtecture, PCIe INTx emulation messages
 * are not supported.  Device drivers supporting PCI/PCI-X/PCIe leaf and nexus
 * devices must provide support for MSI and/or MSI-X interrupts.  All PCIe
 * devices and all but the oldest PCI end devices and bridges support at least
 * MSI interrupts.  Therefore, all fixed interrupt sources on oxide machines are
 * associated with non-PCI devices.
 *
 * Remapping
 *
 * In addition to all of the above, an IOMMU can be used to perform interrupt
 * remapping.  The IOMMU (sometimes IMMU on Intel machines) is part of the
 * northbridge or its conceptual replacement, meaning that interrupts are
 * remapped according to a table programmed into the IOMMU immediately prior to
 * being placed onto the internal APIC bus.  This remapping therefore takes
 * place closer to the CPUs than any IOAPIC, PCIe RC, or other bridging device
 * downstream of the local APIC itself.  The effect of this remapping is that
 * the (apicid, vector) target associated with the interrupt message is
 * virtualised as an index into a per-source-device table.  For PCI sources,
 * the B/D/F is used to select the table; non-PCI sources are identified in a
 * hardware-specific manner.  Each table maps the (apicid, vector) pair onto a
 * new (apicid, vector) pair to which the interrupt should be sent; critically,
 * as all normal APIC messages allow only 8 bits for the APIC ID, the IOMMU
 * supports a 128-bit interrupt routing table entry format in x2APIC mode that
 * allows use of 32-bit destination APIC IDs.  This is necessary to support
 * delivering interrupts to more than 255 logical processors.  Section 2.2.5 of
 * the AMD IOMMU specification provides additional detail.
 *
 * Putting It Together
 *
 * If the above prose isn't doing it for you, consider this block diagram
 * showing the progress of an interrupt from its origin to its delivery as a
 * vectored interrupt at a logical processor (illumos: CPU).  This doesn't cover
 * special interrupt types like NMIs and SMIs, nor does it cover exceptions
 * taken locally on a CPU or generated as IPIs via the local APIC, but it covers
 * all the common cases we're interested in here: interrupts generated by
 * devices downstream of the processor's north- and southbridges, which includes
 * both external devices like PCIe end devices and internal peripherals like
 * SATA and USB controllers, UARTs, and LPC/ISA bridges.  This is a general,
 * conceptual diagram; not every system has all the types of devices shows, most
 * "buses" are really crossbar-switched message-passing networks, and so on.
 * PCIe bridges and root complexes are not shown for end devices using MSI or
 * MSI-X interrupts; numerous other details are also not shown.  Consult the PCI
 * Local Bus specification and AMD PPRs governing NBIO, DF, and FCH
 * functionality.  Non-oxide/non-AMD machines are somewhat different.
 *
 * +-------------------+            +-----------------+     +----------------+
 * |  PCIe End Device  |            | PCIe End Device |     |  MSI-X Table   |
 * | 31     8 7      0 |            | 63            0 |     | 31           0 |
 * | +---------------+ |            | +-------------+ |     | +------------+ |
 * | |  MSI Address  |-+-------+    | |  MSI-X BAR  | |   +-+-| MSI-X Addr | |
 * | +-------+-------+ |       |    | +-------------+ |   | | +------------+ |
 * |         |MsgData|-+---+   |    |        |        |  W| | | MSI-X Msg  | |
 * |         +-------+ |   |   |    +--------+--------+   | | +------------+ |
 * +-------------------+  W|   |W            |            | |  | W           |
 *                         |   |             | R          | +--+-------------+
 *                         v   |             |            |    v  ^ R
 *            =================+=============+============+========= NB data bus
 *                         ^   |         |   |        |   |
 *                         |   v         |   v        |   v
 *            =============+=============+============+============= NB addr bus
 *                 ^       |W            |          | |
 *                W|       |             |          | |
 *         +-------+-------+--------+    |          v v
 *         |       |       |        |    |     +-------+
 *         |    +--------+--------+ |    |     | IOMMU |
 *         |    |  Dest  |  Vect  | | +--+     +-------+
 *         |    +--------+--------+ | |  |         |
 *         | +->|  Dest  |  Vect  | | |  v         v
 *         | |  +--------+--------+ | | +--------------+
 *         | |+>|  Dest  |  Vect  | | | | Device Table |
 *         | || +--------+--------+ | | +--------------+
 *         | || |  Dest  |  Vect  | | |        |
 *         | || +--------+--------+ | |        v
 *         | || 31      0 7       0 | | +------------------+
 *         | ||  Redirection Table  | | | Intr Route Table |
 *         | \\                     | | | +-------+------+ |
 *         | /-------\              | +-+>| x2 ID | Vect | |
 *         |  | | | |    IOAPIC     |   | +-------+------+ |
 *         +--+-^-^-+---------------+   +------+------+----+
 *              | |  \-- Virtual pins          |      |
 *              | |        [0,1,2,3]           |      +----------------+
 *            1 | | 2                          +---------------------+ |
 * +------------+-+--------+                                         | |
 * |            | |        |                                         | |
 * |      +-------------+  |                         +------------+  | |
 * |      | VirtWire 1f |  |                         | Legacy PCI |  | |
 * |      +-------------+  |                         | End Device |  | |
 * |      | VirtWire 1f |  |                         +--------+---+  | |
 * |      +-------------+  |                                  | INTA | |
 * |  +-->| VirtWire  1 |<-+---- FCH::IO::PCI_INTR_INDEX      |      | |
 * |  |   +-------------+  |     FCH::IO::PCI_INTR_DATA       |      | |
 * |  | +>| VirtWire  2 |  |                                  |      | |
 * |  | | +-------------+  |                                  |      | |
 * |  | |   Intr Table     |                                  |      | |
 * | /-----\               |                                  |      | |
 * |  | |      FCH VW Xbar |                                  |      | |
 * +--^-^------------------+     +---------+   +----------+   |      | |
 *    | |                        | PCIe RC |   | PCIe-PCI |   |      | |
 *    | +------------------------+ Swizzle |<--+  bridge  |<--+      | |
 *    | Interrupt Lines          +---------+   | Swizzle  |          | |
 * +----------------+                          +----------+   apicid | | vect
 * | FCH Peripheral |                                                v |
 * +----------------+             CPU addr bus    =====================+======
 *                                                 decode  |           |
 *                                                         |           v
 *                                CPU data bus    =========+==================
 *                                                         | |
 *                                                         | | vect
 *                                +-------------------+    | | data
 *                                | Logical Processor |    | |
 *                                |       x2APIC      |    | |
 *                 IDT            |      +--------+   |    | |
 *                +-------+       |      | 32-bit |<--+----+ |
 *                | Descr |    +--+------| APICID |<--+------+
 *   To           +-------+    |  |      +--------+   |
 *  os/intr.c <---| Descr |<---+  +-------------------+
 *                +-------+ vect
 *                |  ...  |
 *                +-------+<--- IDTR
 *
 * The critical elements to understand here are the potential for three levels
 * of indirection between an interrupt source (here, FCH peripherals and
 * PCI/PCIe end devices) and the logical processor(s) to which the interrupt is
 * to be delivered:
 *
 * 1. The virtual wire crossbar switch controlled by the poorly-named
 *    PCI_INTR_INDEX and PCI_INTR_DATA registers maps fixed hardware-specific
 *    source identifiers onto an IOAPIC virtual wire number.  There is one such
 *    crossbar in each AMD FCH; the destination is always the IOAPIC in the same
 *    FCH.
 * 2. The IOAPIC itself; most AMD processors have additional IOAPICs in the
 *    northbridge which are not discussed here but perform swizzling and deliver
 *    all legacy INTx messages to the virtual-wire crossbar as shown.  MSI/MSI-X
 *    messages are put onto the APIC bus directly unless the IOMMU is in use.
 * 3. From each IOAPIC or MSI/MSI-X end device, the IOMMU can perform remapping
 *    of the messages placed onto the ("northbridge") APIC bus.
 *
 * In reality, the CPU address/data buses and the northbridge buses are
 * effectively the same bus; they are not buses at all but routed networks
 * switched by crossbars in the data fabric, so that they share an address space
 * (for MMIO, RAM, and the APICs) but just as in more familiar networks traffic
 * can be intercepted and modified at each hop.
 *
 * The messages placed onto the conceptual "APIC bus" contain a source ID and a
 * destination vector; the address of these messages selects the destination
 * APIC(s).  Internal hardware-specific implementation provides additional
 * source identification such as is used by the IOMMU.
 *
 * The mechanism for PCI MSI/X is relative straightforward and works the way the
 * standard would lead one to expect.  For fixed interrupts, however, we have
 * nearly unlimited flexibility: each interrupt source has a unique fixed
 * hardware ID that indexes into the FCH's virtual wire crossbar table and is
 * used to select the virtual pin input on the IOAPIC.  The IOAPIC in turn has a
 * redirection table entry for each such virtual pin input that defines 8 bits
 * of the destination APIC ID to be placed in the corresponding APIC message
 * destination address field and an 8-bit vector constituting part of the APIC
 * data payload.  At this point our fixed interrupt has been transformed into an
 * APIC bus message very similar to an interrupt that originated downstream as a
 * PCIe MSI or MSI-X interrupt!  From here, any type of interrupt message may be
 * intercepted and remapped by the IOMMU, allowing us to deliver messages to
 * more than 255 CPUs in physical addressing mode (clustered addressing mode,
 * not used by illumos, is not discussed here) as well as to support advanced
 * features like access control and diversion of interrupts into a guest virtual
 * machine.
 *
 * Interrupt-related Node Properties
 *
 * Coming back to the properties that are associated with device nodes,
 * historically there have been at least two different formats used to describe
 * interrupt usage in IEEE 1275-style properties on i86pc.  The older style
 * named a property "intr" and defined it to contain pairs of integers
 * specifying the ipl and irq number of each interrupt.  Recall from our
 * discussion above that these have little to do with one another: the ipl is a
 * matter of delivery policy (which might be specified by a driver.conf file)
 * while the irq number is primarily an ACPI concept describing either how very
 * old hardware is physically configured or how firmware has configured the
 * virtual wire crossbar switch(es).  The second property style provides a node
 * called "interrupts" which is simply a list of irq numbers.  In either case,
 * the inum or interrupt number used in handles, PSM code, and intr_ops routines
 * indexes into these arrays.  In the newer style, a separate property
 * "interrupt-priorities" provides an array of driver.conf-supplied ipls in
 * which each entry describes the desired delivery ipl for each interrupt
 * source.  If the driver.conf does not supply these, as is typical, a
 * collection of heuristic defaults is used instead, ultimately defaulting to
 * ipl 5.
 *
 * In addition to the 1275 properties, we have several C data types used
 * (sometimes) to store information about a device's interrupt source,
 * intermediate hardware routing, destination, and delivery policy.  These
 * include:
 *
 * struct intrspec
 *
 *   Nominally used "only by old DDI interrupt interfaces", this in fact
 *   pollutes the code in a number of places.  Its members are an ipl, an irq
 *   number incorrectly called a vector, and a handler function pointer.  This
 *   was originally intended to correspond to be a C representation of the
 *   old-style "intr" 1275-style property, much as struct regspec corresponds to
 *   the "reg" property.  The handler function pointer is never invoked, but
 *   there is still a lot of code in other drivers that updates it.
 *
 * struct prop_ispec
 *
 *   This helper type is used to convert the old-style 1275 "intr" property into
 *   C data types.
 *
 * ddi_intr_handle_impl_t [as opaque ddi_intr_handle_t]
 *
 *   Again we have a "vector" member that describes in irq number, not a vector.
 *   We also have ih_private, which is *sometimes* (but by no means always!) an
 *   idhl_plat_t on i86pc and maybe on oxide too.
 *
 * ihdl_plat_t (machdep)
 *
 *   This structure contains the above intrspec as well as kstats and a
 *   performance counter.  Note that this field is often used to hold other data
 *   types specific to various PSM operations.  While it is supposedly specific
 *   to the machine architecture, there are several drivers in uts/common that
 *   make all kinds of assumptions about it.
 *
 * struct ddi_parent_private_data
 *
 *   While parent-private data structures are set as void *, rootnex and the
 *   machdep DDI implementation (here) often want and expect it to be of this
 *   type.  The interrupt-relevant members are par_nintr, a count of interrupt
 *   sources associated with this (child) node, and yet another "obsolete"
 *   instance of struct intrspec, this time an array of them, one representing
 *   each source.
 *
 * On the oxide architecture, we want to simplify this rather dramatically.
 * First, struct intrspec is no longer used by any *current* nexus drivers, but
 * was used in the past by nexus drivers predating BUSO_REV_9.  We don't support
 * any out-of-gate drivers, since we don't support customers installing host
 * software of any kind; therefore we can safely ignore this obsolete use case.
 * The exception is all the internal DDI and PSM code, from which we have
 * removed this structure entirely.
 *
 * XXX Keep going on the brave new world.
 *
 * Interrupts are a gross hack; all interrupt mapping belongs in some
 * combination of the rootnex, apix, and the DDI itself -- upstream of us.  We
 * should be presenting interrupt source information to the rest of the system
 * and letting it set up mappings from source -> IOAPIC pin -> IOMMU -> CPU
 * and vector.  Unfortunately that's going to require a great deal of work, so
 * in the meantime we have this.  In the abstract, there are other sources of
 * interrupts that the FCH's crossbar can assign to IOAPIC virtual pins,
 * including PCI INTx.  The oxide architecture doesn't support those, so we can
 * kind of fudge here and decree that the only fixed interrupt sources are our
 * children.  In general this isn't strictly true and this is a significant
 * barrier to making this driver generic as well as to ever reunifying apix with
 * i86pc or for that matter ever having working interrupt remapping on any
 * platform.  I blame ACPI for this, in the same way I blame the use of pocket
 * calculators for innumeracy: no one has needed to understand how any of this
 * really works because ACPI just hands you an opaque number called an IRQ for
 * each device and all PC OS code pretends these work just like they did on the
 * 8086.  In fact, IRQs are a software construct that have almost no physical
 * meaning at all and should never be exposed outside the interrupt management
 * subsystem.
 *
 * So we're going to manage the first interrupt virtual wire crossbar ourselves.
 * This is really pretty simple on the primary FCH: there are up to 128 possible
 * sources per FCH and each of the FCHs we know about has a single IOAPIC in it
 * with some discoverable number of virtual pins (all known implementations have
 * 24 but up to 256 are possible).  Each source can be unmapped or mapped to a
 * single virtual pin.  Any number of sources may be mapped to the same pin if
 * we wish (historically called IRQ sharing) but we do not wish and as there are
 * relatively few useful sources we currently don't support sharing at all.  If
 * there's no free virtual pin available when a child tries to allocate an
 * interrupt, we fail the request.  We then fill the role performed on PCs by
 * firmware in that we create IRQs, associate them with devices, and pass them
 * into apix when we want to do things with them.  apix knows about the IOAPIC
 * and sets up vectors in the RDT but doesn't know about the virtual wire
 * crossbar (yet).  I'm very sorry for this legacy; we really should have
 * scrapped apix entirely and rewritten it from scratch.
 *
 * The secondary FCH is a complete mystery when it comes to interrupts.  Huashan
 * doesn't allow any secondary FCH peripherals to be used that can ever generate
 * interrupts, which makes sense as it's not at all clear where they go or how
 * they get there.  This isn't necessarily the case on processors containing
 * Songshan, but we don't know whether these peripherals (I2C and I3C in
 * particular) can actually generate interrupts or would have to be used in
 * polled mode on secondary sockets.  We do know that GPIO pins, even AGPIOs,
 * cannot generate interrupts from the second socket, so it's likely that this
 * simply doesn't work and we will end up never exposing any of these children
 * on secondary FCHs.  If it does work, it seems likely that these sources go to
 * the secondary FCH's virtual wire crossbar which in turn directs them into
 * that FCH's IOAPIC.  From there, routing over the DF onto the imaginary APIC
 * bus would be fairly straightforward (this is already how MSI/X interrupts
 * from PCIe devices work).  For now we don't support the secondary FCH's
 * virtual wire crossbar at all, and none of the children that can generate
 * interrupts are enumerated on secondary FCHs.
 */

#include <sys/apix.h>
#include <sys/ddi.h>
#include <sys/dditypes.h>
#include <sys/ddi_impldefs.h>
#include <sys/ddi_subrdefs.h>
#include <sys/ksynch.h>
#include <sys/mach_intr.h>
#include <sys/sunddi.h>
#include <sys/sunndi.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/x86_archext.h>
#include <sys/io/milan/fabric.h>
#include <sys/io/fch.h>
#include <sys/io/fch/gpio.h>
#include <sys/io/fch/i2c.h>
#include <sys/io/fch/i3c.h>
#include <sys/io/fch/iomux.h>
#include <sys/io/fch/ixbar.h>
#include <sys/io/fch/pmio.h>
#include <sys/io/fch/rmtgpio.h>
#include <sys/io/fch/smi.h>
#include <sys/io/fch/uart.h>
#include <sys/io/mmioreg.h>
#include <milan/milan_physaddrs.h>

#define	FCH_PROPNAME_RANGES		"ranges"
#define	FCH_PROPNAME_MODEL		"model"
#define	FCH_PROPNAME_FABRIC_ROLE	"fabric-role"
#define	FCH_FABRIC_ROLE_PRI		"primary"
#define	FCH_FABRIC_ROLE_SEC		"secondary"
#define	FCH_PROPNAME_REG		"reg"
#define	FCH_PROPNAME_INTR		"interrupts"

/* XXX should be generic DDI; see notes in milan_fabric.c. */
typedef enum fch_addrsp {
	FA_NONE,
	FA_LEGACY,
	FA_MMIO,
	FA_INVALID	/* Keep this last; see assertion below. */
} fch_addrsp_t;

#define	FCH_NADDRSP	2

CTASSERT(FCH_NADDRSP == (FA_INVALID - 1));

static inline uint64_t
fch_addrsp_to_bustype(const fch_addrsp_t addrsp)
{
	switch (addrsp) {
	case FA_LEGACY:
		return (1);
	case FA_MMIO:
		return (0);
	default:
		panic("invalid FCH address space %d cannot be translated",
		    addrsp);
	}
}

/*
 * XXX This largely replicates pci_phys_spec but with different addrsp semantics
 * that could be made compatible if we really wanted to.  The fr_addrsp member
 * is really an fch_addrsp_t, but we define it this way to guarantee its size
 * which we rely upon for cramming these into DDI properties.
 */
typedef struct fch_rangespec {
	uint32_t	fr_addrsp;
	uint32_t	fr_physhi;
	uint32_t	fr_physlo;
	uint32_t	fr_sizehi;
	uint32_t	fr_sizelo;
} fch_rangespec_t;

static const uint_t INTS_PER_RANGESPEC =
	(sizeof (fch_rangespec_t) / sizeof (uint32_t));

/*
 * This is the legacy struct regspec that we're forced to use if we want to map
 * our own registers, because our parent is rootnex and doesn't (yet) understand
 * anything beyond rudimentary 32-bit legacy IO or MMIO registers.
 */
static const uint_t INTS_PER_REGSPEC =
	(sizeof (struct regspec) / sizeof (uint32_t));

/*
 * XXX There is a ddi_intrspec_t in the DDI, but it's obsolete; there is a
 * struct intrspec that implements that opaque type in PCI but it's not useful
 * either.  Here's something that is useful, used to communicate with apix.
 *
 * The fi_src is a source index in the FCH's mux downstream of the IOAPIC.  The
 * flags describe how the IOAPIC pin chosen to receive the interrupts should be
 * configured.  For now, we support only one interrupt source per child node,
 * but there is no reason this couldn't be expanded if needed in future since it
 * looks exactly like the register specs.
 *
 * Values for fi_flags are really of type intr_flags_t, from apix.h.  Note that
 * we don't need anywhere near 32, so if we wanted, it would be easy to break
 * that up into a priority and whatever else we might want -- just like the
 * pci_phys_hi member of pci_phys_spec!
 */
typedef struct fch_intrspec {
	uint32_t	fi_flags;
	uint32_t	fi_src;
} fch_intrspec_t;

#define	FCH_INTRSRC_NONE	(uint32_t)-1

static inline uint64_t
fch_rangespec_addr(const fch_rangespec_t *const frp)
{
	uint64_t addr;

	addr = (uint64_t)frp->fr_physhi;
	addr <<= 32;
	addr |= (uint64_t)frp->fr_physlo;

	return (addr);
}

static inline uint64_t
fch_rangespec_size(const fch_rangespec_t *const frp)
{
	uint64_t size;

	size = (uint64_t)frp->fr_sizehi;
	size <<= 32;
	size |= (uint64_t)frp->fr_sizelo;

	return (size);
}

/* XXX see also pci_type_ra2pci() */
static char *
fch_rangespec_to_ndi_ra_type(const fch_rangespec_t *const frp)
{
	switch (frp->fr_addrsp) {
	case FA_LEGACY:
		return (NDI_RA_TYPE_IO);
	case FA_MMIO:
		return (NDI_RA_TYPE_MEM);
	default:
		return (NULL);
	}
}

static uint_t
fch_get_child_reg(dev_info_t *child, fch_rangespec_t **frpp)
{
	uint_t nint, nreg;

	*frpp = NULL;

	if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, child, DDI_PROP_DONTPASS,
	    FCH_PROPNAME_REG, (int **)frpp, &nint) != DDI_SUCCESS) {
		nint = 0;
	}

	if (nint % INTS_PER_RANGESPEC != 0) {
		dev_err(child, CE_WARN, "incomplete or extraneous '%s' entries",
		    FCH_PROPNAME_REG);
	}

	nreg = nint / INTS_PER_RANGESPEC;
	if (nreg == 0 && *frpp != NULL) {
		ddi_prop_free(frpp);
		*frpp = NULL;
	}

	return (nreg);
}

/* XXX duplicates the implementation in pci_memlist.c.  Should be generic. */
static inline uint_t
memlist_count(const memlist_t *ml)
{
	uint_t count = 0;
	while (ml != NULL) {
		++count;
		ml = ml->ml_next;
	}

	return (count);
}

typedef enum fch_child_flags {
	FCF_NONE,
	FCF_PRIMARY = (1 << 0),		/* Usable on primary FCH */
	FCF_SECONDARY = (1 << 1),	/* Usable on secondary FCHs */
} fch_child_flags_t;

typedef struct fch_child_def {
	const char		*fcd_nodename;
	const char		*fcd_desc;
	uint32_t		fcd_addr;	/* DDI node address */
	fch_child_flags_t	fcd_flags;
	const fch_intrspec_t	fcd_intr;
	uint8_t			fcd_nregs;
	const fch_rangespec_t	*fcd_regs;
} fch_child_def_t;

typedef enum fch_kind {
	FK_NONE,
	FK_HUASHAN,
	FK_SONGSHAN
} fch_kind_t;

typedef struct fch_def {
	const char		*fd_nodename;
	const char		*fd_desc;
	fch_kind_t		fd_kind;
	fch_rangespec_t		fd_range_bases[FCH_NADDRSP];
	off_t			fd_sec_bar_off;
	uint32_t		fd_nchildren;
	const fch_child_def_t	*const *fd_children;
} fch_def_t;

typedef enum fch_flags {
	FF_NONE,
	FF_PRIMARY
} fch_flags_t;

/*
 * The interrupt xbar's source number register is 8 bits wide, but the top bit
 * is reserved for a flag indicating whether we want to set up routing to the
 * legacy i8259A-compatible PIC or the integrated IOAPIC.  We only ever use the
 * IOAPIC; the PIC is not supported on oxide machines.  Most of the possible 128
 * source identifiers are unassigned.
 *
 * An IOAPIC can have at most 256 (usually virtual) pins, though in practice all
 * have fewer.  It's an absolute travesty that we need to know anything at all
 * about the IOAPIC but the block comment above addresses that aspect.  There is
 * a lot of legacy goop in the documentation for the IOAPIC, suggesting that a
 * few pins may not be safe to use.  These are marked FIP_F_RESERVED and we
 * don't allocate them; at least a few (likely 8, 14, and 15) are safe to use
 * but for new we'll be extra careful.
 *
 * f_mutex protects both our pin mappings and the underlying xbar's index/data
 * register pair.
 */

typedef enum fch_intr_pin_flag {
	FIP_F_VALID = (1 << 0),
	FIP_F_RESERVED = (1 << 1)
} fch_intr_pin_flag_t;

typedef struct fch_intr_pin {
	uint8_t			fip_idx;
	fch_intr_pin_flag_t	fip_flags;
	uint32_t		fip_src;
} fch_intr_pin_t;

typedef struct fch_ixbar {
	fch_intr_pin_t		*fix_pins;
	ddi_acc_handle_t	fix_reg_hdl;
	uint8_t			*fix_reg;
	uint32_t		fix_npins;
} fch_ixbar_t;

static const uint8_t fch_ioapic_reserved_pins[] = { 0, 1, 2, 8, 12, 14, 15 };

/*
 * State associated with an individual driver instance.
 */
typedef struct fch {
	uint_t			f_inst;
	dev_info_t		*f_dip;
	const fch_def_t		*f_def;
	fch_flags_t		f_flags;
	kmutex_t		f_mutex;
	fch_ixbar_t		f_ixbar;
} fch_t;

/* Global softstate handle */
static void *fch_state;

/*
 * State associated with an individual child node.  This is our parent private
 * data for the child.
 */
typedef struct fch_child {
	fch_t			*fc_parent;
	const fch_child_def_t	*fc_def;
	dev_info_t		*fc_dip;
	fch_intr_pin_t		*fc_intr;
} fch_child_t;

/*
 * Each UART, if present, has 2 sets of registers.  The first is the 16550-ish
 * set of registers plus some additional registers one would expect to find in a
 * UART.  The second is a DMA region that's not normally used; it's not at all
 * clear from the documentation what address space these DMA engines are
 * intended to access and they may just be internal implementation details.
 * Nevertheless they are used address space and even AMD's ACPI tables declare
 * them.  These are all the same on Huashan and Songshan, except that Songshan
 * has only 3 UARTs while Huashan has 4.
 */
static const fch_rangespec_t uart0_regs[] = {
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = FCH_UART_MMIO_APERTURE(0) -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = FCH_UART_SIZE
	},
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = FCH_DMA_MMIO_APERTURE(0) -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = FCH_DMA_SIZE
	}
};
static const fch_rangespec_t uart1_regs[] = {
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = FCH_UART_MMIO_APERTURE(1) -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = FCH_UART_SIZE
	},
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = FCH_DMA_MMIO_APERTURE(1) -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = FCH_DMA_SIZE
	}
};
static const fch_rangespec_t uart2_regs[] = {
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = FCH_UART_MMIO_APERTURE(2) -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = FCH_UART_SIZE
	},
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = FCH_DMA_MMIO_APERTURE(2) -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = FCH_DMA_SIZE
	}
};
static const fch_rangespec_t uart3_regs[] = {
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = FCH_UART_MMIO_APERTURE(3) -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = FCH_UART_SIZE
	},
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = FCH_DMA_MMIO_APERTURE(3) -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = FCH_DMA_SIZE
	}
};

#define	DECL_UART(n, intr)	\
static const fch_child_def_t uart ## n ## _def = {	\
	.fcd_nodename = "dwu",				\
	.fcd_desc = "DesignWare APB UART",		\
	.fcd_addr = (n),				\
	.fcd_flags = FCF_PRIMARY,			\
	.fcd_intr = {					\
		.fi_flags = IF_EDGE | IF_ACTIVE_HIGH,	\
		.fi_src = (intr)			\
	},						\
	.fcd_nregs = ARRAY_SIZE(uart ## n ## _regs),	\
	.fcd_regs = uart ## n ## _regs			\
}

DECL_UART(0, 0x74);
DECL_UART(1, 0x75);
DECL_UART(2, 0x78);
DECL_UART(3, 0x79);

/*
 * There are three banks of "normal" GPIO registers and a fourth bank of
 * "remote" GPIO registers.  Additionally, however, the remote GPIO region also
 * contains its own collection of I/O pinmuxing registers in [0xc0, 0xef] which
 * we want to exclude because they belong to the pinmuxing leaf driver.  All of
 * these are the same on Huashan and Songshan.
 */
static const fch_rangespec_t kczgp_regs[] = {
	/* FCH::GPIO */
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = FCH_GPIO_PHYS_BASE - MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = FCH_GPIO_SIZE
	},
	/* FCH::RMTGPIO bank registers */
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = FCH_RMTGPIO_PHYS_BASE -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = FCH_RMTGPIO_SIZE
	},
	/* FCH::RMTGPIO aggregate control/status registers */
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = FCH_RMTGPIO_AGG_PHYS_BASE -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = FCH_RMTGPIO_AGG_SIZE
	}
};

static const fch_child_def_t kczgp_def = {
	.fcd_nodename = "kczgp",
	.fcd_desc = "KERNCZ GPIO",
	.fcd_addr = 0,
	.fcd_flags = FCF_PRIMARY | FCF_SECONDARY,
	.fcd_intr = {
		.fi_flags = IF_EDGE | IF_ACTIVE_HIGH,
		.fi_src = 0x62
	},
	.fcd_nregs = ARRAY_SIZE(kczgp_regs),
	.fcd_regs = kczgp_regs
};

/*
 * The pinmuxing portion of the GPIO device.  See notes above for why we have
 * these separate regions.
 */
static const fch_rangespec_t kczmux_regs[] = {
	/* FCH::IOMUX */
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = FCH_IOMUX_PHYS_BASE - MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = FCH_IOMUX_SIZE
	},
	/* FCH::RMTGPIO, for pins shared with "remote" GPIO functions */
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = FCH_RMTMUX_PHYS_BASE - MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = FCH_RMTMUX_SIZE
	}
};

static const fch_child_def_t kczmux_def = {
	.fcd_nodename = "kczmux",
	.fcd_desc = "KERNCZ I/O Multiplexor",
	.fcd_addr = 0,
	.fcd_flags = FCF_PRIMARY | FCF_SECONDARY,
	.fcd_intr = {
		.fi_flags = IF_NONE,
		.fi_src = FCH_INTRSRC_NONE
	},
	.fcd_nregs = ARRAY_SIZE(kczmux_regs),
	.fcd_regs = kczmux_regs
};

/*
 * I2C controllers: both Huashan and Songshan have 6 of these, and they're in
 * the same place.  The I2C and I3C peripherals in Songshan share pins but are
 * separate.
 */
static const fch_rangespec_t i2c0_regs[] = {
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = FCH_I2C_MMIO_APERTURE(0) -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = FCH_I2C_SIZE
	}
};

static const fch_rangespec_t i2c1_regs[] = {
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = FCH_I2C_MMIO_APERTURE(1) -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = FCH_I2C_SIZE
	}
};

static const fch_rangespec_t i2c2_regs[] = {
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = FCH_I2C_MMIO_APERTURE(2) -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = FCH_I2C_SIZE
	}
};

static const fch_rangespec_t i2c3_regs[] = {
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = FCH_I2C_MMIO_APERTURE(3) -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = FCH_I2C_SIZE
	}
};

static const fch_rangespec_t i2c4_regs[] = {
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = FCH_I2C_MMIO_APERTURE(4) -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = FCH_I2C_SIZE
	}
};

static const fch_rangespec_t i2c5_regs[] = {
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = FCH_I2C_MMIO_APERTURE(5) -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = FCH_I2C_SIZE
	}
};

#define	DECL_I2C(n, intr)	\
static const fch_child_def_t i2c ## n ## _def = {	\
	.fcd_nodename = "dwi2c",			\
	.fcd_desc = "DesignWare APB I2C Controller",	\
	.fcd_addr = (n),				\
	.fcd_flags = FCF_PRIMARY,			\
	.fcd_intr = {					\
		.fi_flags = IF_EDGE | IF_ACTIVE_HIGH,	\
		.fi_src = (intr)			\
	},						\
	.fcd_nregs = ARRAY_SIZE(i2c ## n ## _regs),	\
	.fcd_regs = i2c ## n ## _regs			\
}

DECL_I2C(0, 0x70);
DECL_I2C(1, 0x71);
DECL_I2C(2, 0x72);
DECL_I2C(3, 0x73);
DECL_I2C(4, 0x76);
DECL_I2C(5, 0x77);

/*
 * Each group of these registers is really two groups, one called FCHI3C that
 * contains a few control registers that include pad controls and one called
 * FCH::I3C (of course!) that contains the peripheral itself.  It's not clear
 * whether we want to present these as two separate regspecs, but each pair does
 * at least share a page of its own.  These are present only on Songshan.
 */
static const fch_rangespec_t i3c0_regs[] = {
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = SONGSHAN_I3C_MMIO_APERTURE(0) -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = SONGSHAN_I3C_SIZE
	}
};

static const fch_rangespec_t i3c1_regs[] = {
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = SONGSHAN_I3C_MMIO_APERTURE(1) -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = SONGSHAN_I3C_SIZE
	}
};

static const fch_rangespec_t i3c2_regs[] = {
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = SONGSHAN_I3C_MMIO_APERTURE(2) -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = SONGSHAN_I3C_SIZE
	}
};

static const fch_rangespec_t i3c3_regs[] = {
	{
		.fr_addrsp = FA_MMIO,
		.fr_physlo = SONGSHAN_I3C_MMIO_APERTURE(3) -
		    MILAN_PHYSADDR_COMPAT_MMIO,
		.fr_sizelo = SONGSHAN_I3C_SIZE
	}
};

#define	DECL_I3C(n, intr)	\
static const fch_child_def_t i3c ## n ## _def = {	\
	.fcd_nodename = "mipii3c",			\
	.fcd_desc = "MIPI I3C Controller",		\
	.fcd_addr = (n),				\
	.fcd_flags = FCF_PRIMARY,			\
	.fcd_intr = {					\
		.fi_flags = IF_EDGE | IF_ACTIVE_HIGH,	\
		.fi_src = (intr)			\
	},						\
	.fcd_nregs = ARRAY_SIZE(i3c ## n ## _regs),	\
	.fcd_regs = i3c ## n ## _regs			\
}

/*
 * Note that the I3C peripherals are the same interrupt sources as the I2C
 * controllers.  That is, these interrupts are shared not at the IOAPIC but at
 * the original source, beyond our ability to separate or distinguish them.
 */
DECL_I3C(0, 0x70);
DECL_I3C(1, 0x71);
DECL_I3C(2, 0x72);
DECL_I3C(3, 0x73);

/*
 * There are additional peripherals that exist in the FCH, most notably an SD
 * controller, an eMMC controller, and an SMBus controller.  There is also an
 * LPC bridge in Huashan that is physically part of the FCH but looks like a PCI
 * device; we don't support LPC/ISA but even if we did it would be a PCI child,
 * not ours.  There are also a number of important registers spread across
 * multiple sub-pagesize blocks that are mostly related to power management,
 * though they also include clocks, GPIO, miscellaneous UART control, SMIs, and
 * more miscellany that one would care to name.  In future we will need to
 * expose that garbage barge *somehow*, even if not to userland, but for now we
 * leave it free and assume that other consumers will access it manually.  For
 * that reason we don't forcibly claim this space for ourselves.  XXX When this
 * is corrected, go back and find those consumers and fix them!
 */

static const fch_child_def_t *const huashan_children[] = {
	&uart0_def,
	&uart1_def,
	&uart2_def,
	&uart3_def,
	&kczgp_def,
	&kczmux_def,
	&i2c0_def,
	&i2c1_def,
	&i2c2_def,
	&i2c3_def,
	&i2c4_def,
	&i2c5_def
};

static const fch_child_def_t *const songshan_children[] = {
	&uart0_def,
	&uart1_def,
	&uart2_def,
	&kczgp_def,
	&kczmux_def,
	&i2c0_def,
	&i2c1_def,
	&i2c2_def,
	&i2c3_def,
	&i2c4_def,
	&i2c5_def,
	&i3c0_def,
	&i3c1_def,
	&i3c2_def,
	&i3c3_def
};

static const fch_def_t fch_defs[] = {
	{
		.fd_nodename = "huashan",
		.fd_desc = "AMD Huashan Fusion Controller Hub",
		.fd_kind = FK_HUASHAN,
		.fd_range_bases = {
			{
				.fr_addrsp = FA_LEGACY,
				.fr_physlo = MILAN_IOPORT_COMPAT_BASE
			},
			{
				.fr_addrsp = FA_MMIO,
				.fr_physlo = MILAN_PHYSADDR_COMPAT_MMIO
			}
		},
		.fd_sec_bar_off =
		    FCH_RELOCATABLE_PHYS_BASE - MILAN_PHYSADDR_COMPAT_MMIO,
		.fd_nchildren = ARRAY_SIZE(huashan_children),
		.fd_children = huashan_children
	},
	/*
	 * XXX These should really be Genoa, or should be renamed to reflect
	 * what is common to both Milan and Genoa.
	 */
	{
		.fd_nodename = "songshan",
		.fd_desc = "AMD Songshan Fusion Controller Hub",
		.fd_kind = FK_SONGSHAN,
		.fd_range_bases = {
			{
				.fr_addrsp = FA_LEGACY,
				.fr_physlo = MILAN_IOPORT_COMPAT_BASE
			},
			{
				.fr_addrsp = FA_MMIO,
				.fr_physlo = MILAN_PHYSADDR_COMPAT_MMIO
			}
		},
		.fd_sec_bar_off =
		    FCH_RELOCATABLE_PHYS_BASE - MILAN_PHYSADDR_COMPAT_MMIO,
		.fd_nchildren = ARRAY_SIZE(songshan_children),
		.fd_children = songshan_children
	}
};

static uint8_t
fch_ixbar_get8(fch_t *fch, uint32_t reg)
{
	fch_ixbar_t *ixp = &fch->f_ixbar;

	ASSERT3U(reg, >=, FCH_IXBAR_IDX);
	ASSERT(MUTEX_HELD(&fch->f_mutex));

	return (ddi_get8(ixp->fix_reg_hdl,
	    ixp->fix_reg + (reg - FCH_IXBAR_IDX)));
}

static void
fch_ixbar_put8(fch_t *fch, uint32_t reg, uint8_t val)
{
	fch_ixbar_t *ixp = &fch->f_ixbar;

	ASSERT3U(reg, >=, FCH_IXBAR_IDX);
	ASSERT(MUTEX_HELD(&fch->f_mutex));

	ddi_put8(ixp->fix_reg_hdl, ixp->fix_reg + (reg - FCH_IXBAR_IDX), val);
}

static fch_intr_pin_t *
fch_ixbar_get_pin_locked(fch_t *fch, uint32_t src)
{
	fch_ixbar_t *ixp = &fch->f_ixbar;
	fch_intr_pin_t *pp;
	uint8_t xbval, pidx;

	ASSERT(MUTEX_HELD(&fch->f_mutex));
	if (src == FCH_INTRSRC_NONE)
		return (NULL);

	ASSERT3U(src, <, FCH_IXBAR_MAX_SRCS);

	xbval = FCH_IXBAR_IDX_SET_SRC(0, (uint8_t)src);
	xbval = FCH_IXBAR_IDX_SET_DST(xbval, FCH_IXBAR_IDX_DST_IOAPIC);
	fch_ixbar_put8(fch, FCH_IXBAR_IDX, xbval);

	xbval = fch_ixbar_get8(fch, FCH_IXBAR_DATA);
	pidx = FCH_IXBAR_PIN_GET(xbval);

	if (pidx == FCH_IXBAR_PIN_NONE)
		return (NULL);

	/*
	 * During initialisation, we set every unused source to INVALID_DST, so
	 * we should never get here with the xbar configured to route a source
	 * to an invalid destination other than the sentinel value.  Because we
	 * are in exclusive control of this xbar, we'll assert on this.
	 */
	ASSERT3U(pidx, <, ixp->fix_npins);
	if (pidx >= ixp->fix_npins)
		return (NULL);

	pp = ixp->fix_pins + pidx;

	/*
	 * Our knowledge of the pin's source should match the hardware's.  We do
	 * not support sharing pins among multiple sources, though the hardware
	 * does.  The mapping should also be valid and the pin not reserved.
	 */
	ASSERT3U(pp->fip_src, ==, src);
	ASSERT3U(pp->fip_flags & FIP_F_VALID, !=, 0);
	ASSERT0(pp->fip_flags & FIP_F_RESERVED);

	return (pp);
}

/*
 * Allocate and set up a destination pin for this child's interrupt.  If it has
 * no interrupt or no pins are available we fail by returning B_FALSE.  This
 * function is idempotent; if the interrupt has already been allocated a pin and
 * that allocation is valid, we succeed without changing anything.
 */
static boolean_t
fch_ixbar_alloc_pin(fch_child_t *child)
{
	fch_t *fch = child->fc_parent;
	fch_ixbar_t *ixp = &fch->f_ixbar;
	fch_intr_pin_t *pp;
	uint32_t src;
	uint8_t xbval;

	src = child->fc_def->fcd_intr.fi_src;

	mutex_enter(&fch->f_mutex);

	if (child->fc_intr != NULL || src == FCH_INTRSRC_NONE) {
		mutex_exit(&fch->f_mutex);
		return (B_FALSE);
	}

	pp = fch_ixbar_get_pin_locked(fch, src);
	if (pp != NULL && (pp->fip_flags & FIP_F_VALID) != 0) {
		ASSERT3U(pp->fip_src, ==, src);
		mutex_exit(&fch->f_mutex);
		return (B_TRUE);
	}

	for (uint8_t pidx = 0; pidx < ixp->fix_npins; pidx++) {
		pp = ixp->fix_pins + pidx;
		if ((pp->fip_flags & (FIP_F_VALID | FIP_F_RESERVED)) == 0) {
			xbval = FCH_IXBAR_IDX_SET_SRC(0, (uint8_t)src);
			xbval = FCH_IXBAR_IDX_SET_DST(xbval,
			    FCH_IXBAR_IDX_DST_IOAPIC);
			fch_ixbar_put8(fch, FCH_IXBAR_IDX, xbval);

			xbval = FCH_IXBAR_PIN_SET(0, pidx);
			fch_ixbar_put8(fch, FCH_IXBAR_DATA, xbval);

			pp->fip_src = src;
			pp->fip_flags |= FIP_F_VALID;
			child->fc_intr = pp;

			mutex_exit(&fch->f_mutex);
			return (B_TRUE);
		}
	}

	mutex_exit(&fch->f_mutex);
	return (B_FALSE);
}

static void
fch_ixbar_blackhole_src(fch_t *fch, uint32_t src)
{
	uint8_t xbval;

	ASSERT3U(src, <, FCH_IXBAR_MAX_SRCS);

	xbval = FCH_IXBAR_IDX_SET_SRC(0, src);
	xbval = FCH_IXBAR_IDX_SET_DST(xbval, FCH_IXBAR_IDX_DST_IOAPIC);
	fch_ixbar_put8(fch, FCH_IXBAR_IDX, xbval);

	xbval = FCH_IXBAR_PIN_SET(0, FCH_IXBAR_PIN_NONE);
	fch_ixbar_put8(fch, FCH_IXBAR_DATA, xbval);

	/*
	 * We never direct any source to the 8259A-compatible PIC, but this code
	 * is used to initialise the ixbar so we want to make sure those
	 * connections are all disabled.  It won't hurt anything to clear them
	 * again when we free an interrupt.
	 */
	xbval = FCH_IXBAR_IDX_SET_SRC(0, src);
	xbval = FCH_IXBAR_IDX_SET_DST(xbval, FCH_IXBAR_IDX_DST_PIC);
	fch_ixbar_put8(fch, FCH_IXBAR_IDX, xbval);

	xbval = FCH_IXBAR_PIN_SET(0, FCH_IXBAR_PIN_NONE);
	fch_ixbar_put8(fch, FCH_IXBAR_DATA, xbval);
}

/*
 * Free the destination pin for this child.  If the source has no configured
 * destination pin, this does nothing.  It is the caller's responsibility to
 * ensure that the interrupt is disabled; it won't be received if it fires after
 * this.
 */
static void
fch_ixbar_free_pin(fch_child_t *child)
{
	fch_t *fch = child->fc_parent;
	fch_intr_pin_t *pp;

	mutex_enter(&fch->f_mutex);

	pp = child->fc_intr;
	if (pp == NULL) {
		mutex_exit(&fch->f_mutex);
		return;
	}

	ASSERT0(pp->fip_flags & FIP_F_RESERVED);
	if ((pp->fip_flags & FIP_F_VALID) == 0) {
		mutex_exit(&fch->f_mutex);
		return;
	}

	ASSERT3U(pp->fip_src, ==, child->fc_def->fcd_intr.fi_src);
	fch_ixbar_blackhole_src(fch, pp->fip_src);
	pp->fip_flags &= ~FIP_F_VALID;
	pp->fip_src = FCH_INTRSRC_NONE;
	child->fc_intr = NULL;

	mutex_exit(&fch->f_mutex);
}

static boolean_t
fch_ixbar_init(fch_t *fch)
{
	fch_ixbar_t *ixp = &fch->f_ixbar;
	uint8_t xbval;
	static ddi_device_acc_attr_t reg_attr = {
		.devacc_attr_version = DDI_DEVICE_ATTR_V1,
		.devacc_attr_endian_flags = DDI_NEVERSWAP_ACC,
		.devacc_attr_dataorder = DDI_STRICTORDER_ACC,
		.devacc_attr_access = DDI_DEFAULT_ACC
	};

	ASSERT((fch->f_flags & FF_PRIMARY) != 0);
	ASSERT0(ixp->fix_npins);

#if 0 /* Symbol resolution can't be satisfied this early in boot. */
	/* Machdep code guarantees that the primary FCH's IOAPIC is index 0. */
	if (apic_io_vectend[0] == apic_io_vectbase[0]) {
		dev_err(fch->f_dip, CE_WARN, "IOAPIC has no IRQs set up!");
		return (B_FALSE);
	}
	ixp->fix_npins = (uint32_t)apic_io_vectend[0] -
	    (uint32_t)apic_io_vectbase[0] + 1;
#endif
	ixp->fix_npins = 24;	/* XXX */

	if (ddi_regs_map_setup(fch->f_dip, 1, (caddr_t *)&ixp->fix_reg, 0, 0,
	    &reg_attr, &ixp->fix_reg_hdl) != DDI_SUCCESS) {
		dev_err(fch->f_dip, CE_WARN, "mapping intr xbar regs failed");
		return (B_FALSE);
	}

	ixp->fix_pins = kmem_zalloc(sizeof (fch_intr_pin_t) * ixp->fix_npins,
	    KM_SLEEP);

	for (uint_t i = 0; i < ARRAY_SIZE(fch_ioapic_reserved_pins); i++) {
		if (fch_ioapic_reserved_pins[i] < ixp->fix_npins) {
			ixp->fix_pins[fch_ioapic_reserved_pins[i]].fip_flags |=
			    FIP_F_RESERVED;
		}
	}

	mutex_enter(&fch->f_mutex);

	/*
	 * Clear the ixbar's pin assignment for each source, then set up our own
	 * internal state.  As much as possible we want the registers themselves
	 * to be the source of truth via fch_ixbar_get_pin() but the ixbar
	 * doesn't provide us any way to get the source(s) assigned to a pin
	 * without walking the entire register space.
	 */
	for (uint_t i = 0; i < ixp->fix_npins; i++) {
		fch_intr_pin_t *pp = ixp->fix_pins + i;

		pp->fip_idx = i;
		pp->fip_src = FCH_INTRSRC_NONE;
	}

	for (uint_t i = 0; i < FCH_IXBAR_MAX_SRCS; i++)
		fch_ixbar_blackhole_src(fch, i);

	/*
	 * We've set up our initial state and the xbar itself.  Now we need to
	 * set up the ancillary control registers.  We want as much as possible
	 * for all interrupt sources to come through the xbar itself; the
	 * mostly-fixed outside sources include SATA/IDE, RTC, PIT (i8254) and
	 * "IMC" which is probably not the memory controller but rather a pile
	 * of legacy kludges for emulating an i8042 via USB (this impression is
	 * strengthened by the use of pins 1 and 12 when enabled).  We use and
	 * want none of these things, ever, and in principle turning off their
	 * bypass bits should allow us to use the corresponding virtual IOAPIC
	 * pins for other things.
	 *
	 * One brief note on the PIT (i8254): the PIT is used to calibrate the
	 * TSC, but we do not otherwise use it and do not enable its interrupt.
	 * Timer interrupts come from the local APIC timer directly and do not
	 * go through the IOAPIC.
	 *
	 * We really don't want the PIC cascading into the IOAPIC at all because
	 * we don't have any PIC interrupt sources we care about (and we don't
	 * configure any of them).  Unfortunately there's no option to do that,
	 * so we set the cascade into pin 2 because it's much less confusing; we
	 * simply reserve pin 2 on the IOAPIC.
	 */
	fch_ixbar_put8(fch, FCH_IXBAR_IDX, FCH_IXBAR_IDX_MISC);
	xbval = fch_ixbar_get8(fch, FCH_IXBAR_DATA);
	xbval = FCH_IXBAR_MISC_SET_PIN15_SRC(xbval, FCH_IXBAR_MISC_PIN1X_XBAR);
	xbval = FCH_IXBAR_MISC_SET_PIN14_SRC(xbval, FCH_IXBAR_MISC_PIN1X_XBAR);
	xbval = FCH_IXBAR_MISC_SET_PIN12_SRC(xbval, FCH_IXBAR_MISC_PIN12_XBAR);
	xbval = FCH_IXBAR_MISC_SET_PIN8_SRC(xbval, FCH_IXBAR_MISC_PIN8_XBAR);
	xbval = FCH_IXBAR_MISC_SET_PIN1_SRC(xbval, FCH_IXBAR_MISC_PIN1_XBAR);
	xbval = FCH_IXBAR_MISC_SET_PIN0_SRC(xbval, FCH_IXBAR_MISC_PIN0_XBAR);
	fch_ixbar_put8(fch, FCH_IXBAR_DATA, xbval);

	fch_ixbar_put8(fch, FCH_IXBAR_IDX, FCH_IXBAR_IDX_MISC0);
	xbval = fch_ixbar_get8(fch, FCH_IXBAR_DATA);
	xbval = FCH_IXBAR_MISC0_SET_PIN12_FILT_EN(xbval, 0);
	xbval = FCH_IXBAR_MISC0_SET_PIN1_FILT_EN(xbval, 0);
	xbval = FCH_IXBAR_MISC0_SET_XBAR_EN(xbval, 1);
	xbval = FCH_IXBAR_MISC0_SET_PINS_1_12_DIS(xbval, 0);
	xbval = FCH_IXBAR_MISC0_SET_CASCADE(xbval,
	    FCH_IXBAR_MISC0_CASCADE_PIN2);
	fch_ixbar_put8(fch, FCH_IXBAR_DATA, xbval);

	mutex_exit(&fch->f_mutex);

	return (B_TRUE);
}

static void
fch_ixbar_fini(fch_t *fch)
{
	fch_ixbar_t *ixp = &fch->f_ixbar;

	if (ixp->fix_npins > 0 && ixp->fix_pins != NULL) {
		kmem_free(fch->f_ixbar.fix_pins,
		    sizeof (fch_intr_pin_t) * ixp->fix_npins);
	}
	ixp->fix_npins = 0;
	ixp->fix_pins = NULL;
}

static dev_info_t *
fch_lookup_child(const fch_t *const fch, const fch_child_def_t *const cdp)
{
	dev_info_t *pdip = fch->f_dip;
	dev_info_t *cdip;
	fch_child_t *child;

	ASSERT(DEVI_BUSY_OWNED(pdip));

	for (cdip = ddi_get_child(pdip); cdip != NULL;
	    cdip = ddi_get_next_sibling(cdip)) {
		child = ddi_get_parent_data(cdip);
		if (child != NULL && child->fc_def == cdp) {
			return (cdip);
		}
	}

	return (NULL);
}

/*
 * A child is usable on a given FCH only if the FCH's role is among those on
 * which the child is supported.  In practice, all children are usable on
 * primary FCHs and only a subset -- possibly empty -- on secondary FCHs.
 */
static boolean_t
fch_child_is_usable(const fch_t *const fch, const fch_child_def_t *const cdp)
{
	return (((fch->f_flags & FF_PRIMARY) != 0 &&
	    (cdp->fcd_flags & FCF_PRIMARY) != 0) ||
	    ((fch->f_flags & FF_PRIMARY) == 0 &&
	    (cdp->fcd_flags & FCF_SECONDARY) != 0));
}

/*
 * Determine whether the register region specified by the request is contained
 * completely within one of the child's register regions described by
 * regs/nregs.  It is the caller's responsibility to ensure that regs and nregs
 * are no less restrictive than what would be returned by fch_get_child_reg.  We
 * choose to require that the base address requested lie within a valid region
 * even if the request length is 0.
 */
static boolean_t
fch_child_reg_valid(const fch_rangespec_t *const req,
    const fch_rangespec_t *const regs, uint_t nregs)
{
	uint64_t req_addr, req_len, req_end;

	req_addr = fch_rangespec_addr(req);
	req_len = fch_rangespec_size(req);
	if (req_len == 0) {
		req_end = req_addr;
	} else {
		req_end = req_addr + (req_len - 1);
	}

	/*
	 * XXX It may not be possible to get here without kernel programmer
	 * error, but just in case this might have come from an untrusted source
	 * somehow, just fail.
	 */
	if (req_end < req_addr)
		return (B_FALSE);

	for (uint_t i = 0; i < nregs; i++) {
		uint64_t addr, end;

		if (req->fr_addrsp != regs[i].fr_addrsp)
			continue;

		addr = fch_rangespec_addr(regs + i);
		end = addr + (fch_rangespec_size(regs + i) - 1);

		ASSERT3U(addr, <, end);

		if (req_addr >= addr && req_end <= end)
			return (B_TRUE);
	}

	return (B_FALSE);
}

static int
fch_bus_map(dev_info_t *dip, dev_info_t *rdip, ddi_map_req_t *mp, off_t offset,
    off_t len, caddr_t *vaddrp)
{
	const fch_rangespec_t *frp_req;
	fch_rangespec_t *frp_child;
	struct regspec64 rs;
	uint_t nregs;
	ddi_map_req_t mr = *mp;

	nregs = fch_get_child_reg(rdip, &frp_child);

	/*
	 * XXX In an ideal world, regspec64 will go the way of the dodo on oxide
	 * and we will make fch_rangespec_t or something similarly flexible,
	 * rigourous, and PCI-compatible its generic replacement as the
	 * rootnex/assumed representation.  We would also have an IOMS as our
	 * parent rather than rootnex itself, the rootnex representing the DF
	 * (or meta-DF if there is more than one), which would also use the more
	 * flexible spec type.  In the meantime, however, we do want to take
	 * advantage of rootnex's generic mapping code which requires that we
	 * translate into regspec64's hardcoded address space ("bus type")
	 * format.
	 */
	switch (mp->map_type) {
	case DDI_MT_REGSPEC:
		frp_req = (const fch_rangespec_t *)mp->map_obj.rp;
		break;
	case DDI_MT_RNUMBER: {
		int reg = mp->map_obj.rnumber;

		if (reg < 0 || reg >= nregs) {
			ddi_prop_free((void *)frp_child);
			return (DDI_ME_RNUMBER_RANGE);
		}

		frp_req = frp_child + reg;
		break;
	}
	default:
		ddi_prop_free((void *)frp_child);
		return (DDI_ME_INVAL);
	}

	if (!fch_child_reg_valid(frp_req, frp_child, nregs)) {
		ddi_prop_free((void *)frp_child);
		return (DDI_ME_REGSPEC_RANGE);
	}

	rs.regspec_bustype = fch_addrsp_to_bustype(frp_req->fr_addrsp);
	rs.regspec_addr = fch_rangespec_addr(frp_req);
	rs.regspec_size = fch_rangespec_size(frp_req);
	ddi_prop_free(frp_child);

	mr.map_type = DDI_MT_REGSPEC;
	mr.map_obj.rp = (struct regspec *)&rs;
	mr.map_flags |= DDI_MF_EXT_REGSPEC;

	return (ddi_map(dip, &mr, (off_t)0, (off_t)0, vaddrp));
}

static int
fch_bus_ctl(dev_info_t *dip, dev_info_t *rdip, ddi_ctl_enum_t ctlop, void *arg,
    void *result)
{
	dev_info_t *cdip;
	fch_child_t *child;

	switch (ctlop) {
	case DDI_CTLOPS_REPORTDEV:
		if (rdip == NULL) {
			return (DDI_FAILURE);
		}
		cmn_err(CE_CONT, "FCH peripheral: %s@%s, %s%d\n",
		    ddi_node_name(rdip), ddi_get_name_addr(rdip),
		    ddi_driver_name(rdip), ddi_get_instance(rdip));
		break;
	case DDI_CTLOPS_INITCHILD: {
		char buf[9];

		cdip = arg;
		if (cdip == NULL) {
			dev_err(dip, CE_WARN, "!no child passed for "
			    "DDI_CTLOPS_INITCHILD");
			return (DDI_FAILURE);
		}

		child = ddi_get_parent_data(cdip);
		if (child == NULL) {
			dev_err(dip, CE_WARN, "!missing child parent data");
			return (DDI_FAILURE);
		}

		/* Can never overrun: 8 chars for 32 bits of hex. */
		(void) snprintf(buf, sizeof (buf), "%x",
		    child->fc_def->fcd_addr);

		ddi_set_name_addr(cdip, buf);
		break;
	}
	case DDI_CTLOPS_UNINITCHILD:
		cdip = arg;
		if (cdip == NULL) {
			dev_err(dip, CE_WARN, "!no child passed for "
			    "DDI_CTLOPS_INITCHILD");
			return (DDI_FAILURE);
		}

		ddi_set_name_addr(cdip, NULL);
		break;
	case DDI_CTLOPS_REGSIZE: {
		off_t *size = (off_t *)result;
		uint_t idx = (uint_t)(*(int *)arg);
		fch_rangespec_t *frp;
		uint_t nreg;

		nreg = fch_get_child_reg(rdip, &frp);
		if (idx >= nreg) {
			return (DDI_FAILURE);
		}

		*size = (off_t)fch_rangespec_size(frp + idx);
		ddi_prop_free(frp);
		return (DDI_SUCCESS);
	}
	case DDI_CTLOPS_NREGS: {
		int *nregp = (int *)result;
		fch_rangespec_t *frp;
		uint_t nreg;

		/*
		 * A child with no registers is useless and every child we
		 * support has at least one, so if there are none something has
		 * gone awry and we treat it as a failure rather than telling
		 * the caller there are zero.
		 */
		nreg = fch_get_child_reg(rdip, &frp);
		if (nreg == 0) {
			return (DDI_FAILURE);
		}

		ddi_prop_free(frp);
		*nregp = nreg;
		return (DDI_SUCCESS);
	}
	default:
		return (ddi_ctlops(dip, rdip, ctlop, arg, result));
	}

	return (DDI_SUCCESS);
}

/*
 * Confusingly, the secondary FCH BAR doesn't hold the address of the base of
 * the entire FCH, only the base of the tiny part it decodes, which in a primary
 * FCH would be at FCH_BASE + 0x18_0000.  We take this into account so that the
 * address we return from here can be used to adjust child addresses in exactly
 * the same manner regardless of whether the FCH is primary or secondary,
 * provided the child can be accessed in this FCH.  The invalid address is
 * returned if there is no valid base for this as.
 */

#define	FCH_ADDR_INVALID	((uint64_t)-1)

static uint64_t
fch_parent_base(const fch_t *fch, const fch_addrsp_t as)
{
	uint64_t addr;
	int asidx = (int)as - 1;

	ASSERT3S(asidx, >, 0);
	ASSERT3S(asidx, <, FCH_NADDRSP);

	if ((fch->f_flags & FF_PRIMARY) != 0) {
		addr = fch_rangespec_addr(&fch->f_def->fd_range_bases[asidx]);
	} else {
		fch_rangespec_t *frp;
		uint_t nint;

		if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, fch->f_dip,
		    DDI_PROP_DONTPASS, FCH_PROPNAME_RANGES, (int **)&frp,
		    &nint) != DDI_SUCCESS) {
			dev_err(fch->f_dip, CE_WARN, "missing '%s' property",
			    FCH_PROPNAME_RANGES);
			return (FCH_ADDR_INVALID);
		}

		if (nint != INTS_PER_RANGESPEC) {
			dev_err(fch->f_dip, CE_WARN, "'%s' property has "
			    "invalid length %d", FCH_PROPNAME_RANGES, nint);
			ddi_prop_free(frp);
			return (FCH_ADDR_INVALID);
		}

		if (frp->fr_addrsp != FA_MMIO) {
			dev_err(fch->f_dip, CE_WARN, "secondary FCH has "
			    "non-MMIO range property");
			ddi_prop_free(frp);
			return (FCH_ADDR_INVALID);
		}

		addr = fch_rangespec_addr(frp) - fch->f_def->fd_sec_bar_off;
		ddi_prop_free(frp);
	}

	return (addr);
}

static int
fch_config_child(fch_t *fch, const fch_child_def_t *const cdp)
{
	dev_info_t *pdip = fch->f_dip;
	dev_info_t *cdip = NULL;
	fch_child_t *child = NULL;
	fch_rangespec_t *frp;
	uint_t i;
	int res;

	ASSERT(DEVI_BUSY_OWNED(pdip));

	cdip = fch_lookup_child(fch, cdp);

	/*
	 * This child already exists.  There is no concept of EEXIST here, so
	 * we'll treat this operation's semantics as idempotent and succeed.
	 */
	if (cdip != NULL)
		return (NDI_SUCCESS);

	if (!fch_child_is_usable(fch, cdp))
		return (NDI_FAILURE);

	/*
	 * Adjust the registers into absolute space, if possible.  If any does
	 * not fit into our ranges, fail.  This shouldn't happen but is possible
	 * if something has gone wrong upstream of us and our ranges are
	 * improperly restricted.  Every defined register region must fit
	 * entirely into a single range, though they need not all fit into the
	 * same range.
	 *
	 * The offset to add to obtain an absolute address is less
	 * straightforward than we might like.  All the child definitions are
	 * specified relative to a notional base address, which is found in the
	 * parent definition as a series of ranges, one per address space
	 * supported by the FCH.  On all currently supported FCHs, this is 0 for
	 * legacy IO port space and MILAN_PHYSADDR_COMPAT_MMIO for MMIO space.
	 * Instead of hardcoding these bases, we allow the possibility that a
	 * future FCH might have a similar collection of peripherals at similar
	 * internal offsets but at a different overall base (ideally in 64-bit
	 * MMIO space, for example) or even at some location specified by a BAR.
	 * That's actually what we have on secondary FCHs already, and they use
	 * that adjustment instead of the fixed one; we don't currently support
	 * routing legacy IO port space to secondary FCHs but it is possible and
	 * could be handled in a similar manner if needed.
	 *
	 * Once we have figured out the correct region for this child relative
	 * to our parent's address space, we attempt to claim it via the
	 * resource allocator, which guarantees we don't have overlapping
	 * or duplicate children; it also would allow for children with BARs if
	 * we ever need them.
	 */
	frp = kmem_zalloc(sizeof (fch_rangespec_t) * cdp->fcd_nregs, KM_SLEEP);

	for (i = 0; i < cdp->fcd_nregs; i++) {
		fch_addrsp_t as = cdp->fcd_regs[i].fr_addrsp;
		uint64_t cdef_addr = fch_rangespec_addr(cdp->fcd_regs + i);
		uint64_t pdef_addr = fch_parent_base(fch, as);
		uint64_t addr, size, end;
		ndi_ra_request_t rr;

		if (pdef_addr == FCH_ADDR_INVALID) {
			dev_err(pdip, CE_WARN, "no valid base address for "
			    "address space %d", (int)as);
			goto fail;
		}

		addr = pdef_addr + cdef_addr;
		if (addr < MAX(pdef_addr, cdef_addr)) {
			dev_err(pdip, CE_WARN, "child '%s@%x' register spec "
			    "%d is beyond the address space", cdp->fcd_nodename,
			    cdp->fcd_addr, i);
			goto fail;
		}

		size = fch_rangespec_size(cdp->fcd_regs + i);
		ASSERT3U(size, !=, 0);
		end = addr + (size - 1);
		if (end < addr) {
			dev_err(pdip, CE_WARN, "child '%s@%x' register spec "
			    "%d ends beyond the address space",
			    cdp->fcd_nodename, cdp->fcd_addr, i);
			goto fail;
		}

		bzero(&rr, sizeof (rr));
		rr.ra_flags = NDI_RA_ALLOC_SPECIFIED;
		rr.ra_len = size;
		rr.ra_addr = addr;

		res = ndi_ra_alloc(pdip, &rr, &addr, &size,
		    fch_rangespec_to_ndi_ra_type(cdp->fcd_regs + i), 0);
		if (res != NDI_SUCCESS) {
			dev_err(pdip, CE_WARN, "child '%s@%x' resource "
			    "%d: base %lx size %lx unavailable",
			    cdp->fcd_nodename, cdp->fcd_addr, as,
			    rr.ra_addr, rr.ra_len);
			goto fail;
		}

		frp[i].fr_addrsp = as;
		frp[i].fr_physhi = (uint32_t)(addr >> 32);
		frp[i].fr_physlo = (uint32_t)addr;
		frp[i].fr_sizehi = (uint32_t)(size >> 32);
		frp[i].fr_sizelo = (uint32_t)size;
	}

	ndi_devi_alloc_sleep(pdip, cdp->fcd_nodename, (pnode_t)DEVI_SID_NODEID,
	    &cdip);
	child = kmem_zalloc(sizeof (fch_child_t), KM_SLEEP);
	child->fc_parent = fch;
	child->fc_def = cdp;
	child->fc_dip = cdip;

	ddi_set_parent_data(cdip, child);

	if (ndi_prop_update_string(DDI_DEV_T_NONE, cdip, FCH_PROPNAME_MODEL,
	    (char *)cdp->fcd_desc) != NDI_SUCCESS ||
	    ndi_prop_update_int_array(DDI_DEV_T_NONE, cdip, FCH_PROPNAME_REG,
	    (int *)frp, cdp->fcd_nregs * INTS_PER_RANGESPEC) != NDI_SUCCESS) {
		goto fail;
	}

	if (cdp->fcd_intr.fi_src != FCH_INTRSRC_NONE &&
	    ndi_prop_update_int_array(DDI_DEV_T_NONE, cdip, FCH_PROPNAME_INTR,
	    (int *)&cdp->fcd_intr, 2) != NDI_SUCCESS) {
		goto fail;
	}

	/*
	 * It's fine if this fails; we may not have a driver for it or it may
	 * need to be added with add_drv etc.  Create the node anyway and let a
	 * subsequent trip through generic code try to bind it again.
	 */
	(void) ndi_devi_bind_driver(cdip, 0);
	goto done;

fail:
	if (cdip != NULL) {
		ddi_set_parent_data(cdip, NULL);
		(void) ndi_devi_free(cdip);
	}

	if (child != NULL) {
		kmem_free(child, sizeof (fch_child_t));
	}

	/* If we got only some of the child's resources, free them. */
	while (i-- != 0) {
		(void) ndi_ra_free(pdip, fch_rangespec_addr(frp + i),
		    fch_rangespec_size(frp + i),
		    fch_rangespec_to_ndi_ra_type(frp + i), 0);
	}

done:
	kmem_free(frp, sizeof (fch_rangespec_t) * cdp->fcd_nregs);

	return (res);
}

static int
fch_config_one(fch_t *fch, const char *const cdrv, const uint32_t inst)
{
	for (uint_t i = 0; i < fch->f_def->fd_nchildren; i++) {
		const fch_child_def_t *const cdp = fch->f_def->fd_children[i];

		if (strcmp(cdp->fcd_nodename, cdrv) == 0 &&
		    cdp->fcd_addr == inst) {
			return (fch_config_child(fch, cdp));
		}
	}

	return (NDI_FAILURE);
}

static void
fch_config_all(fch_t *fch)
{
	for (uint_t i = 0; i < fch->f_def->fd_nchildren; i++)
		(void) fch_config_child(fch, fch->f_def->fd_children[i]);
}

static int
fch_bus_config(dev_info_t *parent, uint_t flags, ddi_bus_config_op_t op,
    void *arg, dev_info_t **childp)
{
	int res, circ;
	fch_t *fch = ddi_get_soft_state(fch_state, ddi_get_instance(parent));

	if (fch == NULL)
		return (NDI_BADHANDLE);

	switch (op) {
	case BUS_CONFIG_ONE:
	case BUS_CONFIG_ALL:
	case BUS_CONFIG_DRIVER:
		ndi_devi_enter(parent, &circ);
		break;
	default:
		return (NDI_FAILURE);
	}

	if (op == BUS_CONFIG_ONE) {
		char *devname = i_ddi_strdup((char *)arg, KM_SLEEP);
		char *cdrv, *caddr;
		u_longlong_t cinst;

		i_ddi_parse_name(devname, &cdrv, &caddr, NULL);

		/*
		 * Instance numbers must fit within an 'int' which means that
		 * any child's index must also fit.  If it doesn't, or if we
		 * don't have any index, puke.
		 */
		if (cdrv == NULL || caddr == NULL ||
		    ddi_strtoull(caddr, NULL, 16, &cinst) != DDI_SUCCESS ||
		    cinst > INT32_MAX) {
			ndi_devi_exit(parent, circ);
			return (NDI_EINVAL);
		}

		res = fch_config_one(fch, cdrv, (uint32_t)cinst);
	} else {
		fch_config_all(fch);
		res = NDI_SUCCESS;
	}

	ndi_devi_exit(parent, circ);

	if (res != NDI_SUCCESS)
		return (res);

	flags |= NDI_ONLINE_ATTACH;

	return (ndi_busop_bus_config(parent, flags, op, arg, childp, 0));
}

static int
fch_bus_unconfig(dev_info_t *parent, uint_t flags, ddi_bus_config_op_t op,
    void *arg)
{
	int res;

	switch (op) {
	case BUS_UNCONFIG_ONE:
	case BUS_UNCONFIG_ALL:
	case BUS_UNCONFIG_DRIVER:
		flags |= NDI_UNCONFIG;
		res = ndi_busop_bus_config(parent, flags, op, arg, NULL, 0);
		if (res != 0)
			return (res);
		break;
	default:
		return (NDI_FAILURE);
	}

	/* XXX teardown? */

	return (NDI_SUCCESS);
}

static int
fch_bus_intr_op(dev_info_t *dip, dev_info_t *rdip, ddi_intr_op_t op,
    ddi_intr_handle_impl_t *hdlp, void *result)
{
	fch_child_t *const child = ddi_get_parent_data(rdip);
	/* XXX */
	extern int (*psm_intr_ops)(dev_info_t *, ddi_intr_handle_impl_t *,
	    psm_intr_op_t, int *);

	ASSERT3P(child, !=, NULL);
	if (child == NULL)
		return (DDI_FAILURE);

	switch (op) {
	case DDI_INTROP_SUPPORTED_TYPES: {
		int *typesp = (int *)result;

		/* Let's build some confidence in the DDI, shall we? */
		ASSERT3P(hdlp->ih_dip, ==, child->fc_dip);

		if (child->fc_def->fcd_intr.fi_src == FCH_INTRSRC_NONE) {
			*typesp = 0;
		} else {
			*typesp = DDI_INTR_TYPE_FIXED;
		}
		return (DDI_SUCCESS);
	}
	case DDI_INTROP_NINTRS: {
		int *nintrp = (int *)result;

		ASSERT3P(hdlp->ih_dip, ==, child->fc_dip);
		ASSERT3S(hdlp->ih_type, ==, DDI_INTR_TYPE_FIXED);
		ASSERT3U(child->fc_def->fcd_intr.fi_src, !=, FCH_INTRSRC_NONE);

		*nintrp = 1;
		return (DDI_SUCCESS);
	}
	case DDI_INTROP_ALLOC: {
		/* XXX Replace this legacy stuff from i86pc and 1275. */
		ihdl_plat_t tmp_ihp;
		struct intrspec ispec;
		int *nallocp = (int *)result;

		ASSERT3P(hdlp->ih_dip, ==, child->fc_dip);
		ASSERT3S(hdlp->ih_type, ==, DDI_INTR_TYPE_FIXED);
		ASSERT0(hdlp->ih_inum);
		ASSERT3S(hdlp->ih_scratch1, ==, 1);
		ASSERT3U(child->fc_def->fcd_intr.fi_src, !=, FCH_INTRSRC_NONE);

		if (psm_intr_ops == NULL)	/* Should never happen. */
			return (DDI_FAILURE);

		if (!fch_ixbar_alloc_pin(child))
			return (DDI_FAILURE);

		/*
		 * XXX apix assumed that intrspec_vec contains not the vector
		 * number but the IRQ number, so we need to fill in all this
		 * stuff with temporary structures.  apix doesn't save these
		 * pointers anywhere so we can just put them on the stack.
		 *
		 * XXX More XXX... nobody anywhere seems to provide locking
		 * here.  Is it possible for a child to have two threads that
		 * race between ALLOC and FREE?  If so, this isn't safe, and we
		 * would need to hold f_mutex across the PSM call.  That would
		 * create some interesting ordering challenges.  If this can't
		 * happen, what prevents it?  In practice I doubt this ever
		 * happens because children are unlikely to create such a race.
		 */
		bzero(&tmp_ihp, sizeof (tmp_ihp));
		bzero(&ispec, sizeof (ispec));
		tmp_ihp.ip_ispecp = &ispec;
		hdlp->ih_private = &tmp_ihp;
		ispec.intrspec_vec = child->fc_intr->fip_idx;
		if (psm_intr_ops(rdip, hdlp, PSM_INTR_OP_ALLOC_VECTORS,
		    (int *)nallocp) != PSM_SUCCESS || *nallocp == 0) {
			return (DDI_FAILURE);
		}

		return (DDI_SUCCESS);
	}
	case DDI_INTROP_GETPRI: {
		int *prip = (int *)result;

		ASSERT3P(hdlp->ih_dip, ==, child->fc_dip);
		ASSERT3S(hdlp->ih_type, ==, DDI_INTR_TYPE_FIXED);
		ASSERT0(hdlp->ih_inum);
		ASSERT3U(child->fc_def->fcd_intr.fi_src, !=, FCH_INTRSRC_NONE);

		/* XXX merge driver.conf properties */
		*prip = 12;
		return (DDI_SUCCESS);
	}
	case DDI_INTROP_SETPRI:
		return (DDI_FAILURE);
	case DDI_INTROP_ENABLE: {
		ihdl_plat_t *ipp = (ihdl_plat_t *)hdlp->ih_private;
		int vec;

		ASSERT3P(hdlp->ih_dip, ==, child->fc_dip);
		ASSERT3S(hdlp->ih_type, ==, DDI_INTR_TYPE_FIXED);
		ASSERT0(hdlp->ih_inum);
		ASSERT3U(child->fc_def->fcd_intr.fi_src, !=, FCH_INTRSRC_NONE);
		/* Allocated by the platform DDI implementation. */
		ASSERT3P(ipp, !=, NULL);

		if (psm_intr_ops == NULL)
			return (DDI_FAILURE);

		if (psm_intr_ops(rdip, hdlp, PSM_INTR_OP_XLATE_VECTOR, &vec) !=
		    PSM_SUCCESS) {
			return (DDI_FAILURE);
		}

		hdlp->ih_vector = vec;

		if (!add_avintr((void *)hdlp, hdlp->ih_pri, hdlp->ih_cb_func,
		    DEVI(rdip)->devi_name, hdlp->ih_vector, hdlp->ih_cb_arg1,
		    hdlp->ih_cb_arg2, &ipp->ip_ticks, rdip)) {
			return (DDI_FAILURE);
		}

		return (DDI_SUCCESS);
	}
	case DDI_INTROP_DISABLE:
		ASSERT3P(hdlp->ih_dip, ==, child->fc_dip);
		ASSERT3S(hdlp->ih_type, ==, DDI_INTR_TYPE_FIXED);
		ASSERT0(hdlp->ih_inum);
		ASSERT3U(child->fc_def->fcd_intr.fi_src, !=, FCH_INTRSRC_NONE);

		rem_avintr((void *)hdlp, hdlp->ih_pri, hdlp->ih_cb_func,
		    hdlp->ih_vector);

		return (DDI_SUCCESS);
	case DDI_INTROP_ADDISR:
	case DDI_INTROP_REMISR:
		/* Nothing to do; the handle contains the handler and args. */
		return (DDI_SUCCESS);
	case DDI_INTROP_FREE:
		ASSERT3P(hdlp->ih_dip, ==, child->fc_dip);
		ASSERT3S(hdlp->ih_type, ==, DDI_INTR_TYPE_FIXED);
		ASSERT0(hdlp->ih_inum);
		ASSERT3U(child->fc_def->fcd_intr.fi_src, !=, FCH_INTRSRC_NONE);

		/*
		 * This can't fail, and it can't be NULL because then we could
		 * never have allocated previously.
		 */
		VERIFY0(psm_intr_ops(rdip, hdlp, PSM_INTR_OP_FREE_VECTORS,
		    NULL));
		fch_ixbar_free_pin(child);
		return (DDI_SUCCESS);
	case DDI_INTROP_GETCAP: {
		int *flagp = (int *)result;

		ASSERT3P(hdlp->ih_dip, ==, child->fc_dip);
		ASSERT3S(hdlp->ih_type, ==, DDI_INTR_TYPE_FIXED);
		ASSERT0(hdlp->ih_inum);
		ASSERT3U(child->fc_def->fcd_intr.fi_src, !=, FCH_INTRSRC_NONE);

		/* XXX Not really, we could do level. */
		*flagp = DDI_INTR_FLAG_EDGE;
		return (DDI_SUCCESS);
	}
	case DDI_INTROP_SETCAP:
		return (DDI_FAILURE);
	case DDI_INTROP_NAVAIL: {
		uint_t *navp = (uint_t *)result;

		ASSERT3P(hdlp->ih_dip, ==, child->fc_dip);
		ASSERT3S(hdlp->ih_type, ==, DDI_INTR_TYPE_FIXED);
		ASSERT0(hdlp->ih_inum);
		ASSERT3U(child->fc_def->fcd_intr.fi_src, !=, FCH_INTRSRC_NONE);

		mutex_enter(&child->fc_parent->f_mutex);
		if (child->fc_intr != NULL)
			*navp = 0;
		else
			*navp = 1;
		mutex_exit(&child->fc_parent->f_mutex);

		return (DDI_SUCCESS);
	}
	default:
		return (i_ddi_intr_ops(dip, rdip, op, hdlp, result));
	}
	return (NDI_FAILURE);
}

static int
fch_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	char *ident = NULL, *role = NULL;
	const fch_def_t *def = NULL;
	fch_t *fch;
	int inst, res;

	switch (cmd) {
	case DDI_ATTACH:
		break;
	case DDI_RESUME:
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}

	ident = ddi_node_name(dip);

	for (uint_t i = 0; i < ARRAY_SIZE(fch_defs); i++) {
		if (strcmp(fch_defs[i].fd_nodename, ident) == 0) {
			def = &fch_defs[i];
			break;
		}
	}

	if (def == NULL) {
		dev_err(dip, CE_WARN, "FCH type '%s' is unsupported", ident);
		return (DDI_FAILURE);
	}

	if (ddi_prop_lookup_string(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
	    FCH_PROPNAME_FABRIC_ROLE, (char **)&role) != DDI_SUCCESS) {
		dev_err(dip, CE_WARN, "'%s' property is missing",
		    FCH_PROPNAME_FABRIC_ROLE);
		return (DDI_FAILURE);
	}

	inst = ddi_get_instance(dip);
	res = ddi_soft_state_zalloc(fch_state, inst);
	VERIFY0(res);

	fch = ddi_get_soft_state(fch_state, inst);
	fch->f_dip = dip;
	fch->f_inst = inst;
	fch->f_def = def;
	mutex_init(&fch->f_mutex, NULL, MUTEX_DRIVER, NULL);

	/*
	 * Set up the interrupt routing xbar such that all sources are directed
	 * to nowhere but the crossbar itself is enabled.  We set up routing
	 * later when children allocate interrupts.  Secondary FCHs supposedly
	 * can't generate interrupts.
	 */
	if (strcmp(role, FCH_FABRIC_ROLE_PRI) == 0) {
		fch->f_flags |= FF_PRIMARY;
		if (!fch_ixbar_init(fch)) {
			ddi_prop_free(role);
			mutex_destroy(&fch->f_mutex);
			ddi_soft_state_free(fch_state, inst);

			return (DDI_FAILURE);
		}
	}
	ddi_prop_free(role);

	VERIFY0(ddi_prop_update_string(DDI_DEV_T_NONE, dip,
	    FCH_PROPNAME_MODEL, (char *)def->fd_desc));

	ddi_report_dev(dip);

	return (DDI_SUCCESS);
}

static int
fch_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	fch_t *fch;
	int inst;

	switch (cmd) {
	case DDI_DETACH:
		break;
	case DDI_SUSPEND:
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}

	inst = ddi_get_instance(dip);
	fch = ddi_get_soft_state(fch_state, inst);
	if (fch == NULL || fch->f_inst != inst || fch->f_dip != dip)
		return (DDI_FAILURE);

	if ((fch->f_flags & FF_PRIMARY) != 0) {
		fch_ixbar_fini(fch);
	}

	mutex_destroy(&fch->f_mutex);
	ddi_soft_state_free(fch_state, fch->f_inst);

	return (DDI_SUCCESS);
}

static struct bus_ops fch_bus_ops = {
	.busops_rev = BUSO_REV,
	.bus_map = fch_bus_map,
	.bus_dma_map = ddi_no_dma_map,
	.bus_dma_allochdl = ddi_no_dma_allochdl,
	.bus_dma_freehdl = ddi_no_dma_freehdl,
	.bus_dma_bindhdl = ddi_no_dma_bindhdl,
	.bus_dma_unbindhdl = ddi_no_dma_unbindhdl,
	.bus_dma_flush = ddi_no_dma_flush,
	.bus_dma_win = ddi_no_dma_win,
	.bus_dma_ctl = ddi_no_dma_mctl,
	.bus_prop_op = ddi_bus_prop_op,
	.bus_ctl = fch_bus_ctl,
	.bus_config = fch_bus_config,
	.bus_unconfig = fch_bus_unconfig,
	.bus_intr_op = fch_bus_intr_op
};

static struct dev_ops fch_dev_ops = {
	.devo_rev = DEVO_REV,
	.devo_getinfo = nodev,
	.devo_identify = nulldev,
	.devo_probe = nulldev,
	.devo_attach = fch_attach,
	.devo_detach = fch_detach,
	.devo_reset = nodev,
	.devo_quiesce = ddi_quiesce_not_needed,
	.devo_bus_ops = &fch_bus_ops
};

static struct modldrv fch_modldrv = {
	.drv_modops = &mod_driverops,
	.drv_linkinfo = "AMD Fusion Controller Hub Nexus Driver",
	.drv_dev_ops = &fch_dev_ops
};

static struct modlinkage fch_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &fch_modldrv, NULL }
};

/*
 * Add the contents of memlist ml to the set of preallocated ranges frp,
 * assuming address space as.  The memlist is freed after conversion and the
 * return value is the number of ranges used, which may be smaller than the
 * number of memlist entries.  This coalesces adjacent memlist spans into a
 * single range and discards empty memlist spans.
 */
static uint_t
memlist_to_ranges(memlist_t *ml, fch_rangespec_t *frp, fch_addrsp_t as)
{
	memlist_t *next;
	uint64_t size = 0, end = 0;
	uint_t ridx;

	for (ridx = 0; ml != NULL; ml = next) {
		next = ml->ml_next;
		if (ml->ml_size == 0) {
			kmem_free(ml, sizeof (memlist_t));
			continue;
		}

		/* Overflowing 64-bit space is always a bug. */
		VERIFY3U(ml->ml_address + (ml->ml_size - 1), >, ml->ml_address);

		size = ml->ml_size;
		end = ml->ml_address + (ml->ml_size - 1);

		frp[ridx].fr_physlo = (uint32_t)ml->ml_address;
		frp[ridx].fr_physhi = (uint32_t)(ml->ml_address >> 32);

		kmem_free(ml, sizeof (memlist_t));

		/* Check for contiguous spans and coalesce. */
		while (next != NULL && next->ml_address == end + 1) {
			ml = next;
			next = ml->ml_next;

			VERIFY3U(size, <, size + ml->ml_size);
			VERIFY3U(end, <, end + ml->ml_size);

			size += ml->ml_size;
			end += ml->ml_size;

			kmem_free(ml, sizeof (memlist_t));
		}

		/* Close out and count this range. */
		frp[ridx].fr_sizelo = (uint32_t)size;
		frp[ridx].fr_sizehi = (uint32_t)(size >> 32);
		frp[ridx].fr_addrsp = as;
		ridx++;
	}

	return (ridx);
}

/*
 * XXX We're going to want to abstract this away so that this driver can be
 * generic, which means making the fabric walkers generic and adding another
 * layer to the subsume logic as in the PCI PRD.  There are ways of figuring
 * this out but they require reaching into a lot of private data.  So for now we
 * practically support only Milan, just like the rest of this architecture, even
 * though this driver itself is mostly capable of supporting many other
 * families.
 *
 * This function is best thought of as a hacked-in parent's bus_config_one().
 * The dip we will operate on is the FCH's itself; the parent is the ioms which
 * has no devinfo node nor driver.
 */
static int
fch_ioms_cb(milan_ioms_t *ioms, void *arg)
{
	dev_info_t *dip = NULL;
	milan_iodie_t *iodie = milan_ioms_iodie(ioms);
	const smn_reg_t enreg = milan_iodie_reg(iodie, D_FCH_PMIO_ALTMMIOEN, 0);
	const smn_reg_t bar = milan_iodie_reg(iodie, D_FCH_PMIO_ALTMMIOBASE, 0);
	memlist_t *ioml, *mmml;
	boolean_t is_primary = B_FALSE;
	int reg[6] = { 0 };
	int res;
	fch_rangespec_t *frp, *ufrp = NULL;
	uint_t rangecount, usable_rangecount = 0;
	ndi_ra_request_t rr;
	uint64_t rr_base, rr_len;
	const char *ident;

	if ((milan_ioms_flags(ioms) & MILAN_IOMS_F_HAS_FCH) == 0)
		return (0);

	if ((milan_iodie_flags(iodie) & MILAN_IODIE_F_PRIMARY) != 0) {
		uint32_t val;

		val = milan_iodie_read(iodie, enreg);
		if (FCH_PMIO_ALTMMIOEN_GET_EN(val) != 0) {
			cmn_err(CE_WARN, "primary FCH has alternate MMIO "
			    "base address set; ignoring");
			return (0);
		}

		is_primary = B_TRUE;
	}

	ioml = milan_fabric_gen_subsume(ioms, IR_GEN_LEGACY);
	mmml = milan_fabric_gen_subsume(ioms, IR_GEN_MMIO);

	rangecount = memlist_count(ioml) + memlist_count(mmml);

	if (rangecount == 0) {
		cmn_err(CE_WARN, "FCH: empty resource memlist");
		return (0);
	}

	switch (chiprev_family(cpuid_getchiprev(CPU))) {
	case X86_PF_AMD_NAPLES:
		ident = "taishan";
		break;
	case X86_PF_AMD_ROME:
	case X86_PF_AMD_MILAN:
		ident = "huashan";
		break;
	case X86_PF_AMD_GENOA:
		ident = "songshan";
		break;
	default:
		/* There may be an FCH but we don't know what it is. */
		return (0);
		break;
	}

	ndi_devi_alloc_sleep(ddi_root_node(), ident, (pnode_t)DEVI_SID_NODEID,
	    &dip);

	frp = kmem_zalloc(sizeof (fch_rangespec_t) * rangecount, KM_SLEEP);

	rangecount = memlist_to_ranges(ioml, frp, FA_LEGACY);
	rangecount += memlist_to_ranges(mmml, frp + rangecount, FA_MMIO);

	/*
	 * At this point, frp/rangecount describes this FCH's notional parent's
	 * available resources not already consumed by PCI.  If this FCH is the
	 * primary one, it will in fact be given the entirety of these
	 * resources, although it doesn't necessarily decode all of them.  The
	 * secondary FCHs are a bit more difficult: they can decode only what we
	 * program into their MMIO BAR, which in present implementations will
	 * support only children consuming the FCH::MISC register space.  In
	 * this case we must find a suitable region, set up the BAR, and adjust
	 * the ranges to reflect what the FCH can see.  We would love to put
	 * this thing in 64-bit space but we cannot because while the BAR has a
	 * 64-bit option, setting it puts the region at 0xffff_ffff_XXXX_0000,
	 * an address this CPU cannot generate.  Sometimes all you can do is
	 * laugh.
	 */
	if (!is_primary) {
		for (uint_t ridx = 0; ridx < rangecount; ridx++) {
			uint32_t val;
			uint64_t addr, size, end;

			if (frp[ridx].fr_addrsp != FA_MMIO)
				continue;
			if (frp[ridx].fr_physhi != 0)
				continue;
			size = fch_rangespec_size(frp + ridx);

			/*
			 * We need a 16-bit-aligned space 8K in size.  If this
			 * range contains such a space, set up the FCH's BAR to
			 * point at it and then throw away all the other ranges
			 * as we cannot use them.
			 */
			addr = fch_rangespec_addr(frp + ridx);
			end = addr + (size - 1);
			addr = P2ROUNDUP(addr,
			    (1U << FCH_PMIO_ALTMMIOBASE_SHIFT));

			if (addr + (FCH_PMIO_ALTMMIOBASE_SIZE - 1) > end)
				continue;

			/*
			 * XXX Here, we would instead have used busra to
			 * allocate this space from the parent if our parent
			 * existed.  It doesn't, so we don't have anywhere to
			 * record that the rest of the space is still available.
			 * At present, there are no other possible consumers, so
			 * we simply throw it all away.
			 */
			frp[ridx].fr_physlo = (uint32_t)addr;
			frp[ridx].fr_sizelo = FCH_PMIO_ALTMMIOBASE_SIZE;

			val = milan_iodie_read(iodie, enreg);
			if (FCH_PMIO_ALTMMIOEN_GET_EN(val) != 0) {
				val = FCH_PMIO_ALTMMIOEN_SET_EN(val, 0);
				milan_iodie_write(iodie, enreg, val);
			}

			val = milan_iodie_read(iodie, bar);
			val = FCH_PMIO_ALTMMIOBASE_SET(val,
			    (uint32_t)addr >> FCH_PMIO_ALTMMIOBASE_SHIFT);
			milan_iodie_write(iodie, bar, val);

			val = FCH_PMIO_ALTMMIOEN_SET_EN(0, 1);
			val = FCH_PMIO_ALTMMIOEN_SET_WIDTH(val,
			    FCH_PMIO_ALTMMIOEN_WIDTH_32);
			milan_iodie_write(iodie, enreg, val);

			ufrp = frp + ridx;
			usable_rangecount = 1;
			break;
		}
	} else {
		ufrp = frp;
		usable_rangecount = rangecount;
	}

	if (ufrp == NULL || usable_rangecount == 0) {
		cmn_err(CE_WARN, "FCH: no resources available");
		goto fail;
	}

	if (ndi_prop_update_int_array(DDI_DEV_T_NONE, dip, FCH_PROPNAME_RANGES,
	    (int *)ufrp, usable_rangecount * INTS_PER_RANGESPEC) !=
	    NDI_SUCCESS) {
		cmn_err(CE_WARN, "FCH: failed to update '%s'",
		    FCH_PROPNAME_RANGES);
		goto fail;
	}

	if (ndi_prop_update_string(DDI_DEV_T_NONE, dip,
	    FCH_PROPNAME_FABRIC_ROLE, is_primary ? FCH_FABRIC_ROLE_PRI :
	    FCH_FABRIC_ROLE_SEC) != NDI_SUCCESS) {
		cmn_err(CE_WARN, "FCH: failed to update '%s'",
		    FCH_PROPNAME_FABRIC_ROLE);
		goto fail;
	}

	/*
	 * Set this FCH's "reg" property.  This is faked up using the legacy
	 * 3x32-bit format that impl_sunbus_name_child() expects, so that this
	 * FCH will end up with a unit address containing the parent IO die's
	 * nodeid.  For the primary die on socket 0, this is always "0".  The
	 * FCH's children include our console device and likely other devices
	 * that may be needed during boot, so it's important that we not rely on
	 * instance numbers when opening a device by pathname.  Thus not only do
	 * all our children have deterministic hardware-derived names, so do we.
	 *
	 * We do have real registers we'd like to be able to map, which follow
	 * the first artificial one.
	 *
	 * XXX Again: setting our name really belongs in our parent's ctl_ops so
	 * that we wouldn't need to rely on the legacy behaviour of
	 * impl_sunbus_name_child()'s interpretation of our "reg" property!
	 */
	reg[0] = 0;
	reg[1] = milan_iodie_node_id(iodie);
	reg[2] = 0;
	reg[3] = 1;	/* legacy I/O */
	reg[4] = FCH_IXBAR_IDX;
	reg[5] = FCH_IXBAR_DATA - FCH_IXBAR_IDX + 1;

	if (ndi_prop_update_int_array(DDI_DEV_T_NONE, dip, FCH_PROPNAME_REG,
	    (int *)reg, ARRAY_SIZE(reg)) != NDI_SUCCESS) {
		cmn_err(CE_WARN, "FCH: failed to update '%s'",
		    FCH_PROPNAME_REG);
		goto fail;
	}

	if (ndi_ra_map_setup(dip, NDI_RA_TYPE_IO) != NDI_SUCCESS) {
		cmn_err(CE_WARN, "FCH: failed to setup legacy I/O map");
		goto fail;
	}
	if (ndi_ra_map_setup(dip, NDI_RA_TYPE_MEM) != NDI_SUCCESS) {
		cmn_err(CE_WARN, "FCH: failed to setup MMIO map");
		(void) ndi_ra_map_destroy(dip, NDI_RA_TYPE_MEM);
		goto fail;
	}

	for (uint_t ridx = 0; ridx < usable_rangecount; ridx++) {
		uint64_t addr, size;

		addr = fch_rangespec_addr(&ufrp[ridx]);
		size = fch_rangespec_size(&ufrp[ridx]);
		res = ndi_ra_free(dip, addr, size,
		    fch_rangespec_to_ndi_ra_type(ufrp + ridx), 0);
		VERIFY3S(res, ==, NDI_SUCCESS);
	}

	/*
	 * Reserve our own registers so we don't accidentally hand them out to
	 * one of our children.
	 */
	for (uint_t ridx = 0; ridx < ARRAY_SIZE(reg) / INTS_PER_REGSPEC;
	    ridx++) {
		bzero(&rr, sizeof (rr));
		rr.ra_flags = NDI_RA_ALLOC_SPECIFIED;
		rr.ra_len = reg[ridx * INTS_PER_REGSPEC + 2];
		rr.ra_addr = reg[ridx * INTS_PER_REGSPEC + 1];
		if (rr.ra_len == 0)
			continue;
		if (ndi_ra_alloc(dip, &rr, &rr_base, &rr_len,
		    (reg[ridx * INTS_PER_REGSPEC] == 0 ? NDI_RA_TYPE_MEM :
		    NDI_RA_TYPE_IO), 0) != NDI_SUCCESS) {
			goto fail;
		}
	}

	if (ndi_devi_bind_driver(dip, 0) == NDI_SUCCESS) {
		goto done;
	}

fail:
	if (dip != NULL) {
		(void) ndi_ra_map_destroy(dip, NDI_RA_TYPE_IO);
		(void) ndi_ra_map_destroy(dip, NDI_RA_TYPE_MEM);
		(void) ndi_devi_free(dip);
	}

done:
	kmem_free(frp, sizeof (fch_rangespec_t) * rangecount);
	return (0);
}

static void
fch_enumerate(int reprobe)
{
	if (reprobe)
		return;

	(void) milan_walk_ioms(fch_ioms_cb, NULL);
}

int
_init(void)
{
	int err;

	/*
	 * It's possible that Hygon Dhyana contains a supported FCH, but not
	 * very likely; it's essentially a Naples part and while there does not
	 * appear to be any documentation available for the FCH one would assume
	 * it contains, it's either similar to the Taishan FCH in Naples that we
	 * don't support or it's something about which we know nothing at all.
	 */
	if (cpuid_getvendor(CPU) != X86_VENDOR_AMD)
		return (ENOTSUP);

	if ((err = mod_install(&fch_modlinkage)) != 0)
		return (err);

	err = ddi_soft_state_init(&fch_state, sizeof (fch_t), 2);
	VERIFY0(err);

	impl_bus_add_probe(fch_enumerate);
	return (0);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&fch_modlinkage, modinfop));
}

int
_fini(void)
{
	impl_bus_delete_probe(fch_enumerate);
	return (mod_remove(&fch_modlinkage));
}
