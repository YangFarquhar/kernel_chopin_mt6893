/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Compatibility shim for the experimental external module build.
 *
 * The source repository omits this vendor OPP header. The overclock module
 * does not reference any symbols from it; it is included transitively by
 * mtk_disp_pmqos.h while importing the MTK display structure declarations.
 * Keep this file empty rather than inventing SoC OPP definitions.
 */
