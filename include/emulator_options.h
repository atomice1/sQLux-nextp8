#pragma once

#ifndef EMULATOR_OPTIONS_H
#define EMULATOR_OPTIONS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef NEXTP8
extern int funcval_mode;
extern uint8_t patch_version;
#endif

int emulatorOptionParse(int argc, char **argv);
void emulatorOptionsRemove(void);
const char *emulatorOptionString(const char *name);
int emulatorOptionInt(const char *name);
int emulatorOptionFlag(const char *name);
int emulatorOptionArgc(void);
const char *emulatorOptionArgv(int idx);

#ifdef __cplusplus
};
#endif

#endif /* EMULATOR_OPTIONS_H */