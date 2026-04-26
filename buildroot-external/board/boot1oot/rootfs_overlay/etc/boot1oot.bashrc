if [ -r /etc/profile ]; then
	. /etc/profile
fi

_boot1oot_complete()
{
	local cur cmd

	cur="${COMP_WORDS[COMP_CWORD]}"
	if [ "$COMP_CWORD" -eq 1 ]; then
		COMPREPLY=( $(compgen -W "mount scan dislocker chntpw unmount init" -- "$cur") )
		return 0
	fi

	cmd="${COMP_WORDS[1]}"
	case "$cmd" in
	mount|dislocker)
		COMPREPLY=( $(compgen -W "-r --read-only -rw --read-write" -- "$cur") )
		;;
	chntpw)
		COMPREPLY=( $(compgen -W "-l --list -u --user" -- "$cur") )
		;;
	*)
		COMPREPLY=()
		;;
	esac
}

complete -F _boot1oot_complete boot1oot
