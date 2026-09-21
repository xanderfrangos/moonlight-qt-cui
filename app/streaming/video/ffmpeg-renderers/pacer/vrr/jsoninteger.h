#pragma once
#include <QJsonValue>
#include <QtGlobal>
#include <cmath>

namespace Vrr13 {
// QJsonValue::toInteger() is Qt 6 only, and the Steam Link SDK ships Qt 5.14.
// The fallback matches Qt 6: whole numbers representable as qint64 convert,
// anything else returns defaultValue.
inline qint64 jsonInteger(const QJsonValue& value, qint64 defaultValue = 0)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return value.toInteger(defaultValue);
#else
    if (!value.isDouble()) return defaultValue;
    const double number = value.toDouble();
    if (number != std::trunc(number) || number < -9223372036854775808.0 ||
        number >= 9223372036854775808.0) {
        return defaultValue;
    }
    return qint64(number);
#endif
}
}
