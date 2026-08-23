#pragma once

static const char* kDefaultLayout = R"INI([Window][WindowOverViewport_11111111]
Pos=0,19
Size=1264,742
Collapsed=0

[Window][Toolbox]
Pos=0,19
Size=315,509
Collapsed=0
DockId=0x00000007,0

[Window][Node Editor]
Pos=317,19
Size=947,742
Collapsed=0
DockId=0x00000002,0

[Window][Variables]
Pos=0,665
Size=315,96
Collapsed=0
DockId=0x00000004,0

[Window][Custom Nodes]
Pos=0,530
Size=315,133
Collapsed=0
DockId=0x00000008,0

[Window][Object Explorer]
Pos=317,19
Size=947,742
Collapsed=0
DockId=0x00000002,2

[Window][Call Logger]
Pos=317,19
Size=947,742
Collapsed=0
DockId=0x00000002,1

[Docking][Data]
DockSpace         ID=0x08BD597D Window=0x1BBC0F80 Pos=0,19 Size=1264,742 Split=Y
  DockNode        ID=0x00000005 Parent=0x08BD597D SizeRef=2560,87 Selected=0xC6859269
  DockNode        ID=0x00000006 Parent=0x08BD597D SizeRef=2560,1288 Split=X
    DockNode      ID=0x00000001 Parent=0x00000006 SizeRef=315,1377 Split=Y Selected=0xB03B8DB7
      DockNode    ID=0x00000003 Parent=0x00000001 SizeRef=134,1180 Split=Y Selected=0x4D0B4ABF
        DockNode  ID=0x00000007 Parent=0x00000003 SizeRef=358,934 Selected=0xB03B8DB7
        DockNode  ID=0x00000008 Parent=0x00000003 SizeRef=358,244 Selected=0x4D0B4ABF
      DockNode    ID=0x00000004 Parent=0x00000001 SizeRef=134,176 Selected=0x6DE9B20C
    DockNode      ID=0x00000002 Parent=0x00000006 SizeRef=2243,1377 CentralNode=1 Selected=0xA5FE7F4E
)INI";
