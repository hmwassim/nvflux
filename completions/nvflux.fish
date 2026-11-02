# fish completion for nvflux
set -l cmds balanced powersave max off status info doctor reload lock help version
complete -c nvflux -f
complete -c nvflux -n "not __fish_seen_subcommand_from $cmds" -a balanced -d 'steady memory, graphics floor'
complete -c nvflux -n "not __fish_seen_subcommand_from $cmds" -a powersave -d 'steady memory, graphics capped'
complete -c nvflux -n "not __fish_seen_subcommand_from $cmds" -a max -d 'memory at full speed'
complete -c nvflux -n "not __fish_seen_subcommand_from $cmds" -a off -d 'driver controls the clocks'
complete -c nvflux -n "not __fish_seen_subcommand_from $cmds" -a status -d 'mode and clocks'
complete -c nvflux -n "not __fish_seen_subcommand_from $cmds" -a info -d 'supported clocks'
complete -c nvflux -n "not __fish_seen_subcommand_from $cmds" -a doctor -d 'check the setup'
complete -c nvflux -n "not __fish_seen_subcommand_from $cmds" -a reload -d 'apply the saved mode again'
complete -c nvflux -n "not __fish_seen_subcommand_from $cmds" -a lock -d 'lock a clock'
complete -c nvflux -n "__fish_seen_subcommand_from lock; and not __fish_seen_subcommand_from memory graphics" -a 'memory graphics'
complete -c nvflux -l temporary -d 'apply without saving'
complete -c nvflux -l gpu -x -d 'one GPU only'
complete -c nvflux -l force -d 'allow a lock that slows the GPU'
complete -c nvflux -l json -d 'machine-readable output'
