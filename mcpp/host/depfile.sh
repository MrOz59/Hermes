# Escape one target or prerequisite for a Ninja depfile.
depfile_word() {
    local path=$1
    path=${path//\\/\\\\}
    path=${path// /\\ }
    path=${path//#/\\#}
    path=${path//\$/\$\$}
    printf '%s' "$path"
}
