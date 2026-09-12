# Move the dictionary flag definitions above the headers that use them.
#
# The metacompiler cannot know whether a word is immediate until it reaches
# the "immediate" after the closing semicolon, so it writes the header first
# and the flags afterwards:
#
#	COLON	forth_dict "\073" semicolon flags_semicolon
#	...
#	END_COLON
#   flags_semicolon = 0
#   flags_semicolon = flags_semicolon + FLAG_IMMEDIATE
#   flags_semicolon = flags_semicolon + FLAG_COMPILE_ONLY
#
# The assembler that came with this in 2009 resolved that forward reference
# to the finished value.  A current one takes the first definition instead,
# so every flags word assembles as zero and nothing in the dictionary is
# immediate: ";" gets compiled into the definition it was meant to close,
# and the interpreter never leaves compile state.  It is a quiet failure --
# the image builds, starts, prints its banner and evaluates arithmetic, and
# only the first colon definition shows that anything is wrong.
#
# Collect each word's flags and emit them all before the first header.

{
  line[NR] = $0
}

/^flags_[A-Za-z0-9_]+ = 0$/ {
  order[++n] = $1
  flags[$1] = ""
  drop[NR] = 1
  next
}

/^flags_[A-Za-z0-9_]+ = flags_[A-Za-z0-9_]+ \+ FLAG_[A-Z_]+$/ {
  flags[$1] = flags[$1] (flags[$1] == "" ? "" : " + ") $NF
  drop[NR] = 1
  next
}

END {
  print "\t;;; dictionary flags, hoisted above their headers"
  for (i = 1; i <= n; i++)
    {
      printf "%s = %s\n", order[i], (flags[order[i]] == "" ? "0" : flags[order[i]])
    }

  for (i = 1; i <= NR; i++)
    {
      if (!(i in drop))
        {
          print line[i]
        }
    }
}
