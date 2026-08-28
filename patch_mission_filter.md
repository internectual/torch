# C fix applied (C++ verified: LIST SIZE=82 via test_click.cs with torch.cfg + t2-linux)
# Script filter (one CTF map) — apply to whichever .cs has the MissionName/onSelect filter:
- Replace %row usage with %id (1185972 pass-through)
- Do NOT cap / single-select; include all missions where $HostType matches selected game type
- If filter uses getRowTextById(%m), iterate all rows, not just index %t
Reference working call: GMH_MissionType.onSelect(%t, "") then dump all rows via getRowTextById(%m)
