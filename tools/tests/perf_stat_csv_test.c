#include <assert.h>
#include <stdio.h>
#include "../linux/tests/perf-stat-csv.h"
int main(void)
{
    const char *bad[]={
        "<not counted>;;instructions:u;300000000;100.00;;",
        "1;;instructions:u;300000000;200.00;;",
        "1;;instructions:u;300000000;0.00;;",
        "1;;instructions:u;0;100.00;;",
        "1;;instructions:u;NaN;100.00;;",
        "1;;instructions:u;300000000;inf;;",
        "inf;;instructions:u;300000000;100.00;;",
        "1;;instructions:u-extra;300000000;100.00;;",
        "1;;instructions:u;300000000garbage;100.00;;",
        "1;;instructions:u;300000000;100.00garbage;;",
        "1;;instructions:u;300000000;100.00",
        "1;instructions:u;300000000;100.00;;"
    };
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)
        assert(!perf_stat_csv_counted(bad[i],"instructions:u"));
    assert(perf_stat_csv_counted("2048922128;;instructions:u;290256012;100.00;;","instructions:u"));
    assert(perf_stat_csv_counted("298.15;msec;task-clock:u;298152769;100.00;1.000;CPUs utilized","task-clock:u"));
    assert(perf_stat_csv_counted("1;;instructions:u;1;0.01;;","instructions:u"));
    puts("PASS: production CSV parser accepts 3 count records and rejects 12 corrupt/impossible records");
}
