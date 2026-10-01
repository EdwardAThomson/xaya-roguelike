#!/usr/bin/env python3
"""Checks that the frontend's version handshake constants match the GSP's.

RULES_VERSION and BANKING_VERSION live in rules.hpp; the frontend pins its
own copies in main.ts.  They are two hand-maintained literals in two
separate repositories, which is exactly the kind of pair that drifts, and
the consequence of drift is not a build error but a warning banner (or a
blocked run) in front of a real player.

This does not decide WHICH way a disagreement should be resolved.  A
frontend that legitimately does not yet implement what a banking bump
covers should stay behind on purpose, and the ROADMAP or a checklist says
so.  It exists so that nobody discovers the gap by deploying.

Exit codes: 0 agree, 1 disagree, 2 could not read one of the files.
"""

import os
import re
import sys

BACKEND = os.path.join (os.path.dirname (os.path.abspath (__file__)),
                        "..", "rules.hpp")
FRONTEND_DEFAULT = "~/Projects/xaya-roguelike-frontend/src/main.ts"


def grab (path, pattern):
  try:
    with open (path) as f:
      text = f.read ()
  except OSError as e:
    print ("cannot read %s: %s" % (path, e), file=sys.stderr)
    return None
  m = re.search (pattern, text)
  if m is None:
    print ("no match for %r in %s" % (pattern, path), file=sys.stderr)
    return None
  return int (m.group (1))


def main ():
  frontend = os.path.expanduser (
      os.environ.get ("ROG_FRONTEND_MAIN", FRONTEND_DEFAULT))

  gspRules = grab (BACKEND, r"constexpr int RULES_VERSION\s*=\s*(\d+)")
  gspBanking = grab (BACKEND, r"constexpr int BANKING_VERSION\s*=\s*(\d+)")
  cliRules = grab (frontend, r"CLIENT_RULES_VERSION\s*=\s*(\d+)")
  cliBanking = grab (frontend, r"CLIENT_BANKING_VERSION\s*=\s*(\d+)")

  if None in (gspRules, gspBanking, cliRules, cliBanking):
    return 2

  print ("            rules  banking")
  print ("GSP         %-6d %d" % (gspRules, gspBanking))
  print ("frontend    %-6d %d" % (cliRules, cliBanking))

  problems = []
  if gspRules != cliRules:
    problems.append (
      "RULES mismatch: this client would REFUSE TO START a run against "
      "this GSP. Both sides must ship together.")
  if gspBanking != cliBanking:
    problems.append (
      "BANKING mismatch: clients keep playing but stop projecting rewards, "
      "so every player sees a warning about a feature they may not have. "
      "Deploy the two together, or leave the frontend behind deliberately "
      "and say so in the checklist.")

  if not problems:
    print ("\nagree")
    return 0

  print ()
  for p in problems:
    print ("MISMATCH: " + p)
  return 1


if __name__ == "__main__":
  sys.exit (main ())
