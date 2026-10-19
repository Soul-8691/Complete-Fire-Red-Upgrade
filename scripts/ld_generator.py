# Credits to mZake
Symbols = open('pokefirered.sym', 'r')
Bpre_ld = open('BPRE.ld', 'w')
Rodata = False

for Symbol in Symbols:
    Name = Symbol[Symbol.rfind(' ') +1:-1]
    Address = Symbol[:8]

    if Name == '.rodata':
        Rodata = True

    Bpre_ld.write(f'{Name} = 0x{Address}')

    if Rodata == False and Address[:2] != '02' and Address[:2] != '03' and Name[0] != 's' and Name[0] != 'g':
        Bpre_ld.write('|1')

    Bpre_ld.write(';\n')

Symbols.close()
Bpre_ld.close()
