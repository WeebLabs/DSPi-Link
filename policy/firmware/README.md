# Firmware command registry

Copies of two files from the DSPi firmware repository, taken at commit
`4eea7df` (firmware 1.1.7 beta 1, 2026-11-17):

| File | Firmware path | What it is |
|------|---------------|------------|
| `dspi_commands.json` | `Documentation/protocol/dspi_commands.json` | Every paged vendor command: page, number, ID, directions, sizes and flags. |
| `legacy_map.json` | `Documentation/protocol/legacy_map.json` | How each 1.1.6 opcode maps onto the paged commands. |

`policy/commands.json` classes every command in `dspi_commands.json`, and
`bridge/esp32/tools/gen_policy.py --check` fails in CI when the two disagree
on which commands and directions exist. When the firmware adds or changes a
command, copy both files again, then add or change the row in
`policy/commands.json` by hand (Link spec 9.3).

The policy table was first derived from the 1.1.6 table with
`legacy_map.json`: every GET is *read*, and each SET took the class of the
1.1.6 opcode that reached it (firmware migration guide, section 13.3.5).
