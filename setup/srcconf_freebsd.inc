#define HAVE_SYSDEP
#define HAVE_KERNEL_FLAGS
static void
check_sysdep (conf_t * conf, struct utsname *un)
{
  strcpy (conf->cplusplus, "g++ -fno-rtti -fno-exceptions -I.");
  strcpy (conf->platform, "i86pc");
}

static void
add_kernel_flags (FILE * f)
{
#if defined(__x86_64__)
  fprintf (f,
	     "CFLAGS += -O3 -DKLD_MODULE -fno-common  -mcmodel=kernel -mno-red-zone  -fno-asynchronous-unwind-tables -ffreestanding\n");
#else
  fprintf (f,
	     "CFLAGS += -O3 -DKLD_MODULE\n");
#endif

}
