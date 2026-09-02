#ifndef DEVSOAK_VERSION_H
#define DEVSOAK_VERSION_H

/* Single source of truth for the release version.
 *
 * A release PR bumps DEVSOAK_VERSION and DEVSOAK_VERSION_DATE here and the
 * matching `Version:` field in devsoak.readme; the tag-driven release
 * workflow (.github/workflows/release.yml) refuses a v<tag> that does not
 * match both.
 *
 * DEVSOAK_VERSION_DATE is the AmigaOS $VER date, <dd>.<mm>.<yyyy> per
 * https://wiki.amigaos.net/wiki/Version_Strings */
#define DEVSOAK_VERSION      "1.0"
#define DEVSOAK_VERSION_DATE "02.09.2026"

/* Embedded AmigaOS version string, findable by the shell `Version` command.
 * The leading "\0" guards against an adjacent string in the binary running
 * into ours; `used` keeps the otherwise-unreferenced constant out of the
 * optimiser's reach. */
#define DEVSOAK_VERSTAG \
    static const char verstag[] __attribute__((used)) = \
        "\0$VER: devsoak " DEVSOAK_VERSION " (" DEVSOAK_VERSION_DATE ")";

#endif
