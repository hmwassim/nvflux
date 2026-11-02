# bash completion for nvflux
_nvflux() {
    local cur prev words cword
    COMPREPLY=()
    cur=${COMP_WORDS[COMP_CWORD]}
    prev=${COMP_WORDS[COMP_CWORD-1]}
    local commands="balanced powersave max off status info doctor reload lock help version"
    case $prev in
    lock) COMPREPLY=($(compgen -W "memory graphics" -- "$cur")); return ;;
    memory|graphics|--gpu|--wait) return ;;
    esac
    if [[ $cur == -* ]]; then
        COMPREPLY=($(compgen -W "--temporary --gpu --force --json --help --version" -- "$cur"))
    elif [[ $COMP_CWORD -eq 1 ]]; then
        COMPREPLY=($(compgen -W "$commands" -- "$cur"))
    fi
}
complete -F _nvflux nvflux
