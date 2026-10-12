__asm__(".symver smoke_versioned_foo_v1, smoke_versioned_foo@VER_1.0");
__asm__(".symver smoke_versioned_foo_v2, smoke_versioned_foo@@VER_2.0");

int smoke_versioned_foo_v1(void)
{
    return 111;
}

int smoke_versioned_foo_v2(void)
{
    return 222;
}

