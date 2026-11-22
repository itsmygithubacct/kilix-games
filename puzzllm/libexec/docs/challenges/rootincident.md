# The Root Incident

An ordinary login on our Ubuntu dispatch machine turned into an unauthorized root session. Reconstruct the incident and return the workload to service with its original data.

Read `/opt/puzzllm/evidence/root.log`. Start at the first unauthorized root audit record, trace the session to its login, and map the original UID to the company directory. Compare the executable in that record with the archived package inventory and the trusted service baseline. The machine also had routine maintenance and an unrelated outstanding update.

Save a JSON object in `~/root-incident.json` with these fields.

| Field | What to submit |
|---|---|
| `account` | Initial company account |
| `login_event` | Original login record ID |
| `root_event` | Unauthorized root audit record ID |
| `component` | Affected package name |
| `advisory` | Ubuntu security notice identifier |
| `repair` | Ordered array of maintenance actions |

The offline advisory for this reconstructed incident is USN-7604-1. CVE-2025-32463 concerns unintended elevated execution in sudo. Replacing the affected binary addresses the weakness. Recovery also requires removing the unauthorized service change and validating dispatch after a fresh boot. [Ubuntu security notice](https://ubuntu.com/security/notices/USN-7604-1)

Use the work-order actions `restore-sudo`, `remove-unapproved-unit`, `restore-dispatch` and `reboot`, in maintenance order. A disposable Ubuntu guest applies the order. Its verifier checks the installed vendor binary, the new boot ID, the service inventory and the original manifest served by the running dispatch workload. Run `~/check_goal_status.sh` for progress.
