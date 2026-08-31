#name: C33 rejects an odd external branch target
#source: branch-align-main.s
#source: branch-align-target.s
#ld: -e 0
#error: .*: relocation truncated to fit: R_C33_JP against symbol `odd_target'.*
