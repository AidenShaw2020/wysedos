/* wyseDOS fixed PC devices. ACPI 1.0, S0/S5 only; no suspend states advertised. */
DefinitionBlock ("dsdt.aml", "DSDT", 1, "WYSEDS", "WYSE3040", 1)
{
    /* PIIX4 S5 type 0, also used by SeaBIOS's APM shutdown outw(0x2000). */
    Name (\_S5, Package (4) { Zero, Zero, Zero, Zero })
    Scope (\_SB)
    {
        // Older Windows already discover the fixed legacy SB16. An ACPI
        // duplicate competes for the same resources on Windows 2000.
        Name (SBPN, Zero)
        Method (_INI, 0, NotSerialized)
        {
            Store (Zero, SBPN)
            If (CondRefOf (\_OSI))
            {
                If (\_OSI ("Windows 2001")) { Store (One, SBPN) }
            }
        }
        Device (PCI0)
        {
            Name (_HID, EisaId ("PNP0A03"))
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                               0, 0, 0, 0, 1)
                IO (Decode16, 0x0CF8, 0x0CF8, 1, 8)
                WordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode, EntireRange,
                        0, 0, 0x0CF7, 0, 0x0CF8)
                WordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode, EntireRange,
                        0, 0x0D00, 0xFFFF, 0, 0xF300)
                DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                             NonCacheable, ReadWrite, 0, 0xA0000, 0xBFFFF, 0, 0x20000)
                DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                             NonCacheable, ReadWrite, 0, 0xE0000000, 0xFEFFFFFF, 0, 0x1F000000)
            })
            /* The PIIX3 ISA bridge is PCI device 1, function 0. ACPI NT
             * enumerates these fixed devices from AML rather than relying
             * on the legacy BIOS keyboard path. */
            Device (ISA)
            {
                Name (_ADR, 0x00010000)
                Device (KBD)
                {
                    Name (_HID, EisaId ("PNP0303"))
                    Name (_STA, 0x0F)
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0060, 0x0060, 1, 1)
                        IO (Decode16, 0x0064, 0x0064, 1, 1)
                        IRQNoFlags () { 1 }
                    })
                }
                Device (SBAU)
                {
                    Name (_HID, EisaId ("PNPB003"))
                    // Mirror the firmware's Sound Blaster enable setting.
                    OperationRegion (SBST, SystemIO, 0x060C, 1)
                    Field (SBST, ByteAcc, NoLock, Preserve) { SBEN, 1 }
                    Method (_STA, 0, NotSerialized)
                    {
                        If (LAnd (\_SB.SBPN, SBEN)) { Return (0x0F) }
                        Return (Zero)
                    }
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0220, 0x0220, 1, 16)
                        IO (Decode16, 0x0330, 0x0330, 1, 2)
                        IO (Decode16, 0x0388, 0x0388, 1, 4)
                        IRQNoFlags () { 5 }
                        DMA (Compatibility, NotBusMaster, Transfer8) { 1 }
                        DMA (Compatibility, NotBusMaster, Transfer16) { 5 }
                    })
                }
                Device (MOU)
                {
                    Name (_HID, EisaId ("PNP0F13"))
                    Name (_STA, 0x0F)
                    Name (_CRS, ResourceTemplate ()
                    {
                        IRQNoFlags () { 12 }
                    })
                }
            }
        }
        Device (SYSR)
        {
            Name (_HID, EisaId ("PNP0C02"))
            Name (_CRS, ResourceTemplate ()
            {
                IO (Decode16, 0x0510, 0x0510, 1, 2)
                IO (Decode16, 0x0600, 0x0600, 1, 64)
            })
        }
    }
}
