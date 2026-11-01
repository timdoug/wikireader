include $(sort $(wildcard $(BR2_EXTERNAL_WIKIREADER_PATH)/package/*/*.mk))

# libtool links a target program or library with a run path to every
# library directory it used outside the system ones: the sysroot's, and a
# library of the same package's build tree.  fix-rpath empties each on the
# target, but the string's length, the build directory's, stays in the
# binary, so the image depended on where it was built.  No target binary
# needs a run path, so link none: the change Fedora makes to every
# package's libtool script, made at the end of each target configure.
define WR_LIBTOOL_NO_RUN_PATH
	$(if $(filter end,$(1)),$(if $(filter configure,$(2)),\
		$(if $(filter target,$($(PKG)_TYPE)),\
			for f in $$(find $($(PKG)_DIR) -name libtool -type f); do \
				grep -q '^hardcode_libdir_flag_spec=' $$f || continue; \
				$(SED) 's/^hardcode_libdir_flag_spec=.*/hardcode_libdir_flag_spec=""/' \
					-e 's/^runpath_var=.*/runpath_var=/' $$f; \
			done)))
endef
GLOBAL_INSTRUMENTATION_HOOKS += WR_LIBTOOL_NO_RUN_PATH
