__asm__(".symver smoke_versioned_foo, smoke_versioned_foo@VER_1.0");
extern int smoke_versioned_foo(void);

int smoke_call_v1(void)
{
    return smoke_versioned_foo();
}

