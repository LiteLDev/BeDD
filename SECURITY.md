# Security Policy

## Scope

Be.D.D. is a testing tool. It hooks input APIs inside the game process and accepts
commands on a local named pipe:

- the pipe rejects remote clients (`PIPE_REJECT_REMOTE_CLIENTS`) and uses the default security
  descriptor, so only the user running the game (and administrators) can write to it;
- whoever can write to the pipe can type and click in that game, including chat commands;
- do not leave the mod installed in a game you play.

## Supported versions

Only the latest release of Be.D.D. receives fixes.

## Reporting a vulnerability

Please do **not** open a public issue for security problems. Report them privately through
[GitHub's private vulnerability reporting](https://github.com/LiteLDev/BeDD/security/advisories/new)
instead. You should get a response within a week. Once a fix is released, the issue is disclosed.
