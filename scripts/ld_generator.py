# Credits to mZake
Symbols = open('pokeemerald.sym', 'r')
Bpee_ld = open('BPEE.ld', 'w')
Rodata = False

for Symbol in Symbols:
    Name = Symbol[Symbol.rfind(' ') +1:-1]
    Address = Symbol[:8]

    if Name == '.rodata':
        Rodata = True

    Bpee_ld.write(f'{Name} = 0x{Address}')

    if Rodata == False and Address[:2] != '02' and Address[:2] != '03' and Name[0] != 's' and Name[0] != 'g':
        Bpee_ld.write('|1')

    Bpee_ld.write(';\n')

Symbols.close()
Bpee_ld.close()
