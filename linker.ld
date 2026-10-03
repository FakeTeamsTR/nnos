ENTRY(start)

SECTIONS
{
	. = 0x1000;
	
	.text :
	{
		KEEP(*(.multiboot))
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