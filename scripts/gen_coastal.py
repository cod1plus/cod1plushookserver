#!/usr/bin/env python3
"""
mp_coastal.map — CoD1
Traced from Battalion 1944 Coastal overhead image + in-game screenshots.
Coord system: X=west→east (0=B Long, 2400=ocean wall)
              Y=south→north (0=south, 3200=north, Axis at S, Allies at N)
              Z=height (0=ground, 256=ceiling)
Scale: ~4 units/pixel from 1000-px overhead.
"""

def box(x1,y1,z1,x2,y2,z2,tw,tf=None,tc=None):
    tf=tf or tw; tc=tc or "textures/common/caulk"
    return "{\n"+"\n".join([
        f"( {x1} {y1} {z1} ) ( {x1} {y2} {z1} ) ( {x2} {y2} {z1} ) {tf} 0 0 0 1 1",
        f"( {x2} {y2} {z2} ) ( {x1} {y2} {z2} ) ( {x1} {y1} {z2} ) {tc} 0 0 0 1 1",
        f"( {x1} {y1} {z1} ) ( {x2} {y1} {z1} ) ( {x2} {y1} {z2} ) {tw} 0 0 0 1 1",
        f"( {x2} {y2} {z1} ) ( {x1} {y2} {z1} ) ( {x1} {y2} {z2} ) {tw} 0 0 0 1 1",
        f"( {x1} {y2} {z1} ) ( {x1} {y1} {z1} ) ( {x1} {y1} {z2} ) {tw} 0 0 0 1 1",
        f"( {x2} {y1} {z1} ) ( {x2} {y2} {z1} ) ( {x2} {y2} {z2} ) {tw} 0 0 0 1 1",
    ])+"\n}"

def void(x1,y1,z1,x2,y2,z2):
    c="textures/common/caulk"; return box(x1,y1,z1,x2,y2,z2,c,c,c)

def wall_with_door(xa,ya,xb,yb,z1,z2,dx1,dx2,tw,dh=112):
    """Wall from (xa,ya)→(xb,yb) with a door opening [dx1,dx2] along X or Y."""
    B=[]
    # Determine if wall is along X (ya==yb) or Y (xa==xb)
    if ya==yb:   # horizontal wall
        if xa<dx1: B.append(box(xa,ya,z1,dx1,yb,z2,tw))
        B.append(box(dx1,ya,dh,dx2,yb,z2,tw))  # linteau
        if dx2<xb: B.append(box(dx2,ya,z1,xb,yb,z2,tw))
    else:        # vertical wall
        if ya<dx1: B.append(box(xa,ya,z1,xb,dx1,z2,tw))
        B.append(box(xa,dx1,dh,xb,dx2,z2,tw))  # linteau
        if dx2<yb: B.append(box(xa,dx2,z1,xb,yb,z2,tw))
    return B

def ent(cls,x,y,z,**kw):
    L=["{",f'"classname" "{cls}"',f'"origin" "{x} {y} {z}"']
    for k,v in kw.items(): L.append(f'"{k}" "{v}"')
    L.append("}"); return "\n".join(L)

GND="textures/mp_carentan/ext_street"
WE ="textures/mp_carentan/building_ext"
WI ="textures/mp_carentan/building_int"
SKY="textures/sky/overcast"
C  ="textures/common/caulk"

WH=256; DH=112

B=[]

# ══════════════════════════════════════════════════════════
#  WORLD SEAL  — X 0..2400 / Y 0..3200 playable
# ══════════════════════════════════════════════════════════
B+=[ void(-512,-512,-128, 2912,3712, 0),                    # sub-floor
     box( -512,-512, WH,  2912,3712, WH+128, SKY,SKY,SKY), # skybox
     void(-512,-512,  0,     0,3712, WH),                   # W outer
     void(2400,-512,  0,  2912,3712, WH),                   # E outer
     void(-512,-512,  0,  2912,   0, WH),                   # S outer
     void(-512,3200,  0,  2912,3712, WH),                   # N outer
     box(    0,    0,-16,  2400,3200, 0, GND,GND,C) ]       # ground

# ══════════════════════════════════════════════════════════
#  OCEAN  — fill X 2080-2400 solid for Y 760-2460
#           seafront parapet at X 2020-2080 (low wall, h=96)
# ══════════════════════════════════════════════════════════
B+=[ void(2080, 760, 0, 2400, 2460, WH),          # ocean void
     box(2020, 760, 0, 2080, 2460, 96, WE) ]      # seafront parapet

# Solid fills connecting ocean zone to outer walls
B+=[ void(2080,    0, 0, 2400,  760, WH),         # S of seafront
     void(2080, 2460, 0, 2400, 3200, WH) ]        # N of seafront

# ══════════════════════════════════════════════════════════
#  1. AXIS SPAWN  (X 800-1440 / Y 100-760)
# ══════════════════════════════════════════════════════════
B+=[ void(  0, 100, 0,  800,  760, WH),           # W fill
     void(1440, 100, 0, 2400,  760, WH),           # E fill
     box( 800,  100, 0, 1440,  160, WH, WE),       # S back wall
     box( 800,  160, 0,  860,  700, WH, WE),       # W wall
     box(1380,  160, 0, 1440,  700, WH, WE),       # E wall
     void(800,  100, WH-2, 1440, 760, WH) ]        # roof
# N wall (2 exits at X 940-1020 and X 1220-1300)
B+=[ box( 800, 700, 0,  940, 760, WH, WE),
     box(1020, 700, 0, 1220, 760, WH, WE),
     box(1300, 700, 0, 1440, 760, WH, WE),
     box( 940, 700, DH,1020, 760, WH, WE),        # linteau L
     box(1220, 700, DH,1300, 760, WH, WE) ]       # linteau R

# ══════════════════════════════════════════════════════════
#  2. SPAWN STREET  (Y 760-1200, mostly open)
#  Far left: B Long starts; far right: Back A void
# ══════════════════════════════════════════════════════════
B+=[ void(  0, 760, 0,  440, 1100, WH),           # far-left before B Long
     void(2000, 760, 0, 2400, 1200, WH),           # right fill (Back A hasn't opened yet)
     box( 900,  840, 0, 1080,  900,  48, WE),      # sandbag cover center
     box(1320,  960, 0, 1480, 1020,  64, WE) ]     # kubel/vehicle

# ══════════════════════════════════════════════════════════
#  3. A SITE BUILDING  (X 1340-1940 / Y 1020-1500)
# ══════════════════════════════════════════════════════════
# S wall — door X 1480-1560
B+=[ box(1340,1020, 0, 1480,1080, WH, WE),
     box(1560,1020, 0, 1940,1080, WH, WE),
     box(1480,1020, DH,1560,1080, WH, WE) ]
# N wall — door X 1480-1560
B+=[ box(1340,1440, 0, 1480,1500, WH, WE),
     box(1560,1440, 0, 1940,1500, WH, WE),
     box(1480,1440, DH,1560,1500, WH, WE) ]
# E wall (→ Back A) — door Y 1160-1280
B+=[ box(1880,1080, 0, 1940,1160, WH, WE),
     box(1880,1280, 0, 1940,1440, WH, WE),
     box(1880,1160, DH,1940,1280, WH, WE) ]
# W wall (→ Broken Wall) — door Y 1140-1260
B+=[ box(1340,1080, 0, 1400,1140, WH, WE),
     box(1340,1260, 0, 1400,1440, WH, WE),
     box(1340,1140, DH,1400,1260, WH, WE) ]
# Interior divider — door Y 1200-1280
B+=[ box(1400,1080, 0, 1680,1200, WH, WI),
     box(1400,1280, 0, 1680,1440, WH, WI),
     box(1400,1200, DH,1680,1280, WH, WI) ]
B.append(void(1340,1020,WH-2,1940,1500,WH))       # roof

# ══════════════════════════════════════════════════════════
#  4. BACK A  (X 1940-2020 / Y 1020-1560)
#  Small area E of A site, connects to Seafront corridor
# ══════════════════════════════════════════════════════════
B+=[ box(1940,1020, 0, 2020,1080, WH, WE),        # S wall
     box(1940,1080, 0, 2000,1500, WH, WE),        # W wall (A site side)
     void(1940,1020,WH-2,2020,1560,WH) ]          # roof

# ══════════════════════════════════════════════════════════
#  5. SEAFRONT CORRIDOR  (X 1940-2020 / Y 1560-2460)
#  Narrow passage along the ocean parapet
# ══════════════════════════════════════════════════════════
B.append(box(1940,1560, 0, 2000,2460, WH, WE))    # W wall (buildings side)
# E side = parapet at X 2020 (already defined); ocean void at X 2080

# ══════════════════════════════════════════════════════════
#  6. SHORTCUT  (X 1700-1940 / Y 2020-2140)
#  Cross-passage from Cobbles/Car Park to Seafront
# ══════════════════════════════════════════════════════════
B+=[ box(1700,2020, 0, 1940,2080, WH, WE),        # S wall
     box(1700,2080, 0, 1760,2140, WH, WE),        # W wall
     # N wall with door X 1820-1900
     box(1700,2080, 0, 1820,2140, WH, WE),
     box(1900,2080, 0, 1940,2140, WH, WE),
     box(1820,2080, DH,1900,2140, WH, WE) ]       # linteau

# ══════════════════════════════════════════════════════════
#  7. CAR PARK / RAT / BARRELS  (X 1440-1960 / Y 2200-2460)
#  Open area between Courtyard and Allied spawn
# ══════════════════════════════════════════════════════════
B+=[ box(1440,2200, 0, 1500,2460, WH, WE),        # W wall
     box(1500,2380, 0, 1960,2460, WH, WE),        # N wall (joins Allied spawn S)
     box(1560,2240, 0, 1700,2310, 56, WE),        # car/cover
     box(1800,2260, 0, 1860,2320, 64, WE) ]       # barrel cluster

# ══════════════════════════════════════════════════════════
#  8. ALLIED SPAWN  (X 1760-2280 / Y 2460-2900)
# ══════════════════════════════════════════════════════════
B+=[ void(   0,2460, 0, 1760,2900, WH),           # W fill
     void(2280,2460, 0, 2400,2900, WH),           # E fill
     box(1760,2840, 0, 2280,2900, WH, WE),        # N back wall
     box(1760,2460, 0, 1820,2840, WH, WE),        # W wall
     box(2220,2460, 0, 2280,2840, WH, WE),        # E wall
     void(1760,2460,WH-2,2280,2900,WH) ]          # roof
# S wall — 2 exits (X 1860-1940 and X 2080-2160)
B+=[ box(1760,2460, 0, 1860,2520, WH, WE),
     box(1940,2460, 0, 2080,2520, WH, WE),
     box(2160,2460, 0, 2280,2520, WH, WE),
     box(1860,2460, DH,1940,2520, WH, WE),        # linteau L
     box(2080,2460, DH,2160,2520, WH, WE) ]       # linteau R

# ══════════════════════════════════════════════════════════
#  9. COURTYARD  (X 1080-1700 / Y 1960-2400)
#  Large central open square — statue in the middle
# ══════════════════════════════════════════════════════════
# W wall
B.append(box(1080,1960, 0, 1140,2400, WH, WE))
# N wall — full (Car Park connects via opening above Y=2380)
B.append(box(1140,2340, 0, 1700,2400, WH, WE))
# E wall — S half + N half (gap Y 2180-2280 toward Car Park/Shortcut)
B+=[ box(1640,1960, 0, 1700,2180, WH, WE),
     box(1640,2280, 0, 1700,2340, WH, WE) ]
# S entrance from Cobbles (gap X 1220-1440)
B+=[ box(1140,1960, 0, 1220,2020, WH, WE),        # S left stub
     box(1440,1960, 0, 1640,2020, WH, WE) ]       # S right stub

# Statue base (octagonal approximated as box + smaller box)
B+=[ box(1330,2120,  0, 1470,2260, 20, WE),       # plinth
     box(1355,2145, 20, 1445,2235, 68, WE),       # pedestal
     box(1383,2173, 68, 1417,2207,196, WE) ]      # angel column (thin)
# Low sandbag ring around statue
B+=[ box(1280,2090, 0, 1520,2115, 36, WE),
     box(1280,2285, 0, 1520,2310, 36, WE),
     box(1280,2115, 0, 1305,2285, 36, WE),
     box(1495,2115, 0, 1520,2285, 36, WE) ]

# ══════════════════════════════════════════════════════════
#  10. COBBLES / LAMP POST / MID ARCH  (X 1080-1700 / Y 1800-1960)
#  Street leading into Courtyard from south
# ══════════════════════════════════════════════════════════
B+=[ box(1080,1800, 0, 1140,1960, WH, WE),        # W wall
     box(1640,1800, 0, 1700,1960, WH, WE),        # E wall
     # Mid Arch — archway (wall with opening)
     box(1140,1880, 0, 1320,1940, WH, WE),        # arch L pillar
     box(1420,1880, 0, 1640,1940, WH, WE),        # arch R pillar
     box(1320,1880, DH,1420,1940, WH, WE),        # arch linteau
     # Lamp post
     box(1460,1830, 0, 1480,1850, 220, WE) ]      # lamp post pillar

# ══════════════════════════════════════════════════════════
#  11. HALLWAY  (X 1140-1480 / Y 1620-1800)
#  Key chokepoint below Cobbles
# ══════════════════════════════════════════════════════════
B+=[ box(1140,1620, 0, 1200,1800, WH, WE),        # W wall
     box(1420,1620, 0, 1480,1800, WH, WE) ]       # E wall

# ══════════════════════════════════════════════════════════
#  12. APPARTMENT  (X 1480-1940 / Y 1500-1960)
#  Building with window + backdoor, E of Hallway
# ══════════════════════════════════════════════════════════
B+=[ box(1480,1500, 0, 1940,1560, WH, WE),        # S wall
     box(1880,1560, 0, 1940,1900, WH, WE),        # E wall
     void(1480,1500,WH-2,1940,1960,WH) ]          # roof
# N wall (backdoor X 1640-1720)
B+=[ box(1480,1900, 0, 1640,1960, WH, WE),
     box(1720,1900, 0, 1940,1960, WH, WE),
     box(1640,1900, DH,1720,1960, WH, WE) ]       # backdoor linteau
# W wall — window at Y 1680-1780, Z 48-104
B+=[ box(1480,1560,  0, 1540,1680, WH, WE),       # W wall A (solid)
     box(1480,1680,  0, 1540,1780,  48, WE),      # below window
     box(1480,1680,104, 1540,1780, WH, WE),       # above window
     box(1480,1780,  0, 1540,1900, WH, WE) ]      # W wall B (solid)

# ══════════════════════════════════════════════════════════
#  13. BROKEN WALL / MID TANK  (X 1200-1480 / Y 1440-1640)
#  Open area with rubble wall + tank
# ══════════════════════════════════════════════════════════
B+=[ box(1200,1440, 0, 1260,1640, WH, WE),        # W wall
     box(1420,1440, 0, 1480,1640, WH, WE),        # E wall
     # Broken wall (rubble, gap at X 1330-1380)
     box(1260,1540, 0, 1330,1590, 80, WE),
     box(1380,1540, 0, 1420,1590, 80, WE),
     # Tank
     box(1260,1450, 0, 1400,1530, 72, WE),        # tank hull
     box(1290,1470,72, 1370,1510, 96, WE) ]       # turret

# ══════════════════════════════════════════════════════════
#  14. CONNECTOR + CONNECTOR HOUSE  (X 1000-1260 / Y 1360-1640)
# ══════════════════════════════════════════════════════════
B+=[ box(1000,1360, 0, 1260,1420, WH, WE),        # S wall
     box(1000,1360, 0, 1060,1580, WH, WE),        # W wall
     void(1000,1360,WH-2,1260,1640,WH) ]          # roof
# N wall (door X 1080-1160)
B+=[ box(1000,1580, 0, 1080,1640, WH, WE),
     box(1160,1580, 0, 1260,1640, WH, WE),
     box(1080,1580, DH,1160,1640, WH, WE) ]       # N door linteau
# E wall (door Y 1460-1560, facing Hallway)
B+=[ box(1200,1420, 0, 1260,1460, WH, WE),
     box(1200,1560, 0, 1260,1580, WH, WE),
     box(1200,1460, DH,1260,1560, WH, WE) ]       # E door linteau

# ══════════════════════════════════════════════════════════
#  15. B MAIN / B SITE  (X 540-1000 / Y 1460-1980)
# ══════════════════════════════════════════════════════════
# S wall (door X 680-760)
B+=[ box( 540,1460, 0,  680,1520, WH, WE),
     box( 760,1460, 0, 1000,1520, WH, WE),
     box( 680,1460, DH, 760,1520, WH, WE) ]
# N wall (→ B Arch, door X 680-760)
B+=[ box( 540,1920, 0,  680,1980, WH, WE),
     box( 760,1920, 0, 1000,1980, WH, WE),
     box( 680,1920, DH, 760,1980, WH, WE) ]
# E wall (→ Connector, door Y 1640-1760)
B+=[ box( 940,1520, 0, 1000,1640, WH, WE),
     box( 940,1760, 0, 1000,1920, WH, WE),
     box( 940,1640, DH,1000,1760, WH, WE) ]
# W wall
B.append(box(540,1520, 0, 600,1920, WH, WE))
B.append(void(540,1460,WH-2,1000,1980,WH))        # roof

# ══════════════════════════════════════════════════════════
#  16. B HOUSE / WINDOWS / DARK  (X 380-600 / Y 1700-2080)
#  Building with windows facing B Main
# ══════════════════════════════════════════════════════════
# S wall (door X 460-540)
B+=[ box( 380,1700, 0,  460,1760, WH, WE),
     box( 540,1700, 0,  600,1760, WH, WE),
     box( 460,1700, DH, 540,1760, WH, WE) ]
B+=[ box( 380,2020, 0,  600,2080, WH, WE),        # N wall
     box( 380,1760, 0,  440,2020, WH, WE),        # W wall (B Long side)
     void(380,1700,WH-2,600,2080,WH) ]            # roof
# E wall — TWO windows facing B Main
B+=[ box( 540,1760,  0,  600,1840,  32, WE),      # below window 1 (headshot)
     box( 540,1760, 80,  600,1840, WH, WE),       # above window 1
     box( 540,1840,  0,  600,1920, WH, WE),       # solid section
     box( 540,1920,  0,  600,2020,  48, WE),      # below window 2
     box( 540,1920,104,  600,2020, WH, WE) ]      # above window 2
# B Stair (interior stairs)
B.append(box(440,1960, 0, 540,2020, 40, WI))

# ══════════════════════════════════════════════════════════
#  17. B STEPS + WOODEN  (X 540-1000 / Y 1200-1460)
#  Connects Spawn Street to B Main
# ══════════════════════════════════════════════════════════
B+=[ box( 540,1200, 0, 1000,1260, WH, WE),        # S wall
     box( 540,1260, 0,  600,1460, WH, WE),        # W wall
     box( 940,1260, 0, 1000,1460, WH, WE),        # E wall
     box( 640,1300, 0,  860,1360,  56, WE),       # wooden platform
     box( 600,1380, 0,  660,1460,  32, WE),       # step 1
     box( 600,1380,32,  660,1460,  64, WE) ]      # step 2

# ══════════════════════════════════════════════════════════
#  18. B LONG CORRIDOR  (X 0-440 / Y 1100-2280)
#  Left-side flanking route (long)
# ══════════════════════════════════════════════════════════
B+=[ box(  0,1100, 0,  440,1160, WH, WE),         # S wall
     box(380,1100, 0,  440,2280, WH, WE),         # E wall (faces interior)
     box(  0,2220, 0,  440,2280, WH, WE),         # N wall (→ Barricade)
     box(  0,1440, 0,  340,1500,  48, WE),        # sandbag cover 1
     box(  0,1820, 0,  340,1880,  48, WE) ]       # sandbag cover 2

# ══════════════════════════════════════════════════════════
#  19. B ARCH  (X 580-960 / Y 1980-2280)
#  Arcaded building passage (Gothic arches — see screenshot)
# ══════════════════════════════════════════════════════════
B+=[ box( 580,1980, 0,  640,2280, WH, WE),        # W solid wall
     box( 900,1980, 0,  960,2280, WH, WE),        # E wall
     # N wall (door X 720-820)
     box( 640,2220, 0,  720,2280, WH, WE),
     box( 820,2220, 0,  960,2280, WH, WE),
     box( 720,2220, DH, 820,2280, WH, WE) ]       # N door linteau
# Arch columns (pillars approximated, gap = arch opening beneath)
for py in [1980, 2040, 2100, 2160]:
    B.append(box(640, py, 0, 680, py+24, WH, WE))  # column

# ══════════════════════════════════════════════════════════
#  20. GARDEN  (X 940-1140 / Y 1980-2280)
#  Vegetation area between B Arch and Courtyard
# ══════════════════════════════════════════════════════════
B+=[ box( 940,1980, 0, 1140,2040, WH, WE),        # S wall
     # Low garden walls (cover)
     box( 960,2100, 0, 1020,2160,  40, WE),
     box(1080,2100, 0, 1120,2160,  40, WE),
     # Tree (approximated as thin box column)
     box(1040,2180, 0, 1080,2220, 180, WE) ]      # tree trunk

# ══════════════════════════════════════════════════════════
#  21. BARRICADE  (X 580-960 / Y 2280-2560)
#  Upper-left route segment
# ══════════════════════════════════════════════════════════
B+=[ box( 580,2280, 0,  640,2560, WH, WE),        # W wall
     box( 900,2280, 0,  960,2560, WH, WE),        # E wall
     box( 640,2500, 0,  900,2560, WH, WE),        # N wall (dead-end)
     box( 660,2360, 0,  900,2400,  64, WE) ]      # barricade cover

# ══════════════════════════════════════════════════════════
#  22. HEADSHOT / JUMPBOX  (X 340-560 / Y 1580-1760)
#  Far-left alcove
# ══════════════════════════════════════════════════════════
B.append(box(360,1620, 0, 440,1700, 64, WE))      # jumpbox

# ══════════════════════════════════════════════════════════
#  ASSEMBLE WORLDSPAWN
# ══════════════════════════════════════════════════════════
out=["// mp_coastal - CoD1",
     "// Traced from Battalion 1944 Coastal (overhead + screenshots)",
     "// Retexture in Radiant — textures are mp_carentan/* placeholders",
     ""]
out.append("{")
out.append('"classname" "worldspawn"')
out.append('"ambient" "50"')
out.append('"sundirection" "70 30 0"')
out.append('"sunlight" "100"')
out.append('"suncolor" "1 0.92 0.75"')
out.append("")
for b in B:
    out.append(b); out.append("")
out.append("}")
out.append("")

# ══════════════════════════════════════════════════════════
#  ENTITIES
# ══════════════════════════════════════════════════════════

# Allied spawns — angle 270 (facing W, into the map)
for x,y,a in [(2000,2700,270),(2060,2740,270),(1920,2720,225),
              (1960,2780,270),(2020,2800,270),(1900,2800,225)]:
    out.append(ent("info_player_allies",x,y,32,angle=str(a))); out.append("")

# Axis spawns — angle 0 (facing N, into the map)
for x,y,a in [(1020,300,0),(1100,340,0),(940,340,0),
              (1060,420,0),(1140,400,45),(960,400,315)]:
    out.append(ent("info_player_axis",x,y,32,angle=str(a))); out.append("")

# Bomb sites
out.append(ent("mp_searchanddestroy_bombzone_a",1580,1260,32)); out.append("")
out.append(ent("mp_searchanddestroy_bombzone_b", 760,1720,32)); out.append("")

# DM spawns (spread across all major zones)
for x,y,a in [
    (1020, 380,  0),(1060, 460,  0),              # Axis spawn
    (1000, 900,  0),(1400, 900,  0),(1700,900,0), # Spawn Street
    (1580,1260,  0),(1200,1260,  0),              # A site / approach
    ( 760,1720, 90),(1000,1500,  0),              # B site
    (1260,1700,  0),(1440,1870,270),              # Hallway / Appartment
    (1380,2190,  0),(1560,2300,270),              # Courtyard / Car Park
    (1960,2700,270),(1960,1800,270),              # Allied spawn / Seafront
    ( 200,1600,  0),( 200,1960,  0),              # B Long
    ( 760,2100,  0),( 760,2420,  0),              # B Arch / Barricade
]:
    out.append(ent("info_player_deathmatch",x,y,32,angle=str(a))); out.append("")

# Lights
for lx,ly,lz in [
    (1060, 380,200),(1100, 900,200),(1580,1260,200),
    ( 760,1720,200),(1200,1700,200),(1380,2190,200),
    (1600,2340,200),(1960,2720,200),(1960,1800,200),
    ( 200,1700,200),( 760,2100,200),(1440,1700,200),
]:
    out.append(ent("light",lx,ly,lz,light="250",_color="1 0.92 0.75")); out.append("")

print("\n".join(out))
