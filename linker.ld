ENTRY(start)

SECTIONS
{
	. = 0x1000;
	
	.text :
	{
		*(.multiboot2_header)
		*(.text)
	}
	
	.data :
	{
		*(.data)
	}
	
	.bss :
	{
		*(.bss)
		*(COMMON)
	}
}