#pragma once

#define MFD_CLOEXEC         0x0001U
#define MFD_ALLOW_SEALING   0x0002U
#define MFD_HUGETLB         0x0004U

#define MFD_HUGE_SHIFT      26
#define MFD_HUGE_MASK       0x3f
#define MFD_HUGE_64KB       (16 << MFD_HUGE_SHIFT)
#define MFD_HUGE_512KB      (19 << MFD_HUGE_SHIFT)
#define MFD_HUGE_1MB        (20 << MFD_HUGE_SHIFT)
#define MFD_HUGE_2MB        (21 << MFD_HUGE_SHIFT)
#define MFD_HUGE_8MB        (23 << MFD_HUGE_SHIFT)
#define MFD_HUGE_16MB       (24 << MFD_HUGE_SHIFT)
#define MFD_HUGE_32MB       (25 << MFD_HUGE_SHIFT)
#define MFD_HUGE_256MB      (28 << MFD_HUGE_SHIFT)
#define MFD_HUGE_512MB      (29 << MFD_HUGE_SHIFT)
#define MFD_HUGE_1GB        (30 << MFD_HUGE_SHIFT)
#define MFD_HUGE_2GB        (31 << MFD_HUGE_SHIFT)
#define MFD_HUGE_16GB       (34 << MFD_HUGE_SHIFT)

#define F_SEAL_SEAL     0x0001
#define F_SEAL_SHRINK   0x0002
#define F_SEAL_GROW     0x0004
#define F_SEAL_WRITE    0x0008
#define F_SEAL_FUTURE_WRITE 0x0010
