#pragma once

#include <QtGlobal>

#ifdef Q_OS_LINUX

#include <QByteArray>
#include <QString>
#include <cstdint>

// Shader bytecode is read from the user's installed Lossless Scaling DLL and
// translated in memory. No licensed data is stored by Moonlight.
struct Ls1Shaders {
    QByteArray stage1;
    QByteArray stage2;
    QByteArray stage3;
    QByteArray reconstruct;
    QString translator;
};

QString findLosslessScalingDll(const QString& configuredPath);
bool loadLs1Shaders(const QString& dllPath, int variant, Ls1Shaders* shaders,
                    QString* error);
bool patchLs1OutputFormat(QByteArray* shader, uint32_t spirvFormat);

#endif
