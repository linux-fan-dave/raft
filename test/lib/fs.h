/**
 * File-system related test utilties.
 */

#ifndef TEST_FS_H
#define TEST_FS_H

#include "munit.h"

/**
 * Create a temporary test directory.
 */
char *test_dir_setup(const MunitParameter params[]);

/**
 * Recursively remove a temporary directory.
 */
void test_dir_tear_down(char *dir);

#endif /* TEST_IO_H */
