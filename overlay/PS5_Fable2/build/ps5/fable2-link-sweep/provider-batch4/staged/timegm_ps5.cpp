
#include <cstdint>
#include <ctime>

namespace {

int64_t floor_div(int64_t x, int64_t d) {
    int64_t q = x / d;
    if (x % d < 0) {
        --q;
    }
    return q;
}

int64_t days_from_civil(int64_t year, int64_t month, int64_t day) {
    year -= month <= 2;

    const int64_t era = floor_div(year, 400);
    const int64_t yoe = year - era * 400;

    const int64_t adjusted_month =
        month + (month > 2 ? -3 : 9);

    const int64_t doy =
        (153 * adjusted_month + 2) / 5;

    const int64_t doe =
        yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * 146097 + doe - 719468 + day - 1;
}

}

extern "C" time_t timegm(struct tm* value) {
    if (!value) {
        return static_cast<time_t>(-1);
    }

    int64_t year =
        static_cast<int64_t>(value->tm_year) + 1900;

    int64_t month = value->tm_mon;
    const int64_t year_offset = floor_div(month, 12);

    year += year_offset;
    month -= year_offset * 12;
    month += 1;

    const int64_t days =
        days_from_civil(year, month, value->tm_mday);

    const int64_t seconds =
        days * 86400
        + static_cast<int64_t>(value->tm_hour) * 3600
        + static_cast<int64_t>(value->tm_min) * 60
        + value->tm_sec;

    return static_cast<time_t>(seconds);
}
