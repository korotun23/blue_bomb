#!/usr/bin/env python3
"""Génère company_ids.cpp / company_ids.h à partir du CSV du Bluetooth SIG.
Usage : python gen_company_ids.py Bluetooth-Company-Identifiers.csv
"""
import csv, re, sys

src = sys.argv[1] if len(sys.argv) > 1 else "Bluetooth-Company-Identifiers.csv"
rows = []
with open(src, encoding="utf-8") as f:
    for r in csv.reader(f):
        if len(r) >= 2:
            name = r[1].strip()
            name = re.sub(r"\s*\(\s*\(\)\s*\)", "", name)   # ex. "Espressif Incorporated ( () )"
            name = name.replace("\\", "\\\\").replace('"', '\\"')
            rows.append((int(r[0], 16), name))

rows.sort()                                  # indispensable pour la recherche dichotomique
ids = [r[0] for r in rows]
assert len(ids) == len(set(ids)), "identifiants en double"

with open("company_ids.h", "w", encoding="utf-8") as h:
    h.write("""#pragma once
#include <stdint.h>

// Retourne le nom du constructeur, ou nullptr si l'identifiant est inconnu.
const char *companyName(uint16_t id);
""")

with open("company_ids.cpp", "w", encoding="utf-8") as c:
    c.write('// Fichier généré par gen_company_ids.py : ne pas modifier à la main.\n')
    c.write('#include "company_ids.h"\n\n')
    c.write("struct CompanyEntry { uint16_t id; const char *name; };\n\n")
    c.write("// const => placé en flash, pas en RAM. Trié par id croissant.\n")
    c.write("static const CompanyEntry kCompanies[] = {\n")
    for i, n in rows:
        c.write(f'    {{0x{i:04X}, "{n}"}},\n')
    c.write("};\n\n")
    c.write("""const char *companyName(uint16_t id)
{
    int lo = 0;
    int hi = (int)(sizeof(kCompanies) / sizeof(kCompanies[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (kCompanies[mid].id == id) return kCompanies[mid].name;
        if (kCompanies[mid].id < id)  lo = mid + 1;
        else                          hi = mid - 1;
    }
    return nullptr;
}
""")
print(len(rows), "entrées générées")
