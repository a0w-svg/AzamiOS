/* Validate upstream perf stat -x ';' output for one requested event. */
#pragma once
#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline int perf_stat_csv_counted(const char *line, const char *event)
{
    char *end;
    double count=strtod(line,&end);
    if(end==line || *end!=';' || !isfinite(count) || count<=0) return 0;
    const char *unit=strchr(end+1,';');
    const char *event_end=unit ? strchr(unit+1,';') : NULL;
    if(!event_end || (size_t)(event_end-unit-1)!=strlen(event) ||
       memcmp(unit+1,event,strlen(event))) return 0;
    const char *runtime=event_end+1;
    double running=strtod(runtime,&end);
    if(end==runtime || *end!=';' || !isfinite(running) || running<=0) return 0;
    const char *percentage=end+1;
    double percent=strtod(percentage,&end);
    return end!=percentage && *end==';' && isfinite(percent) && percent>0 && percent<=100;
}
