BEGIN {
    if (symbol == "") symbol = "default_config"
    print "#include <stddef.h>"
    print "const char " symbol "[] ="
}

/^[^;#]/ {
    gsub(/["\\]/, "\\\\&")
    print "\t\"" $0 "\\n\""
}

END {
    print ";"
    print "const size_t " symbol "_len = sizeof(" symbol ") - 1;"
}
