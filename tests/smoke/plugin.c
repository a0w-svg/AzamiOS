extern int smoke_start_value(void);
static int initialized;

__attribute__((constructor)) static void plugin_init(void)
{
    initialized = smoke_start_value();
}

int smoke_plugin_value(int value)
{
    return initialized + value;
}
