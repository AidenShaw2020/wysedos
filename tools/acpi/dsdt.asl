/* wyseDOS fixed PC devices. ACPI 1.0, S0/S5 only; no suspend states advertised. */
DefinitionBlock ("dsdt.aml", "DSDT", 1, "WYSEDS", "WYSE3040", 1)
{
    /* PIIX4 S5 type 0, also used by SeaBIOS's APM shutdown outw(0x2000). */
    Name (\_S5, Package (4) { Zero, Zero, Zero, Zero })
    Scope (\_SB)
    {
        Device (PCI0)
        {
            Name (_HID, EisaId ("PNP0A03"))
            Name (_ADR, Zero)
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
