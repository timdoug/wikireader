# generate an application header

BEGIN {
        NAME_LENGTH = 32
        n = 0
        name[0] = "No Name"
}

END {
        printf("SAMO%c%c%c%c", n, 0, 0, 0)
        for (i = 0; i < n; ++i) {
                title = substr(name[i], 1, NAME_LENGTH)
                printf("%s", title)
                for (j = length(title); j < NAME_LENGTH; ++j) {
                        printf("%c", 0)
                }
        }
}

/^[[:space:]]*#[[:space:]]*define[[:space:]]+APPLICATION_TITLE[[:digit:]]*[[:space:]]+/ {
        line = $0
        sub("^.*APPLICATION_TITLE[[:digit:]]*[[:space:]]*\"", "", line)
        sub("\".*$", "", line)
        if ("" != line) {
                name[n] = line
                n = n + 1
        }
}
