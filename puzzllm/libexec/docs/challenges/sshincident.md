# The SSH Incident

Our SSH server was breached. Identify the company account used by the intruder, cite the evidence, and return the service to our engineers.

Read `/opt/puzzllm/evidence/ssh.log`. It contains read-only copies of the account directory, access gateway events and SSH host events. Several engineers share an address. Trace the connection marked by the server's integrity alarm to its authenticated gateway account. The records identify a credential holder's account. They cannot establish who was at the keyboard.

Save a JSON object in `~/ssh-incident.json` with these fields.

| Field | What to submit |
|---|---|
| `account` | Company account name from the directory |
| `gateway_event` | Gateway record ID |
| `server_event` | SSH host record ID |
| `repair` | Ordered array of maintenance actions |

The offline advisory for this historical case is USN-6859-1 (CVE-2024-6387). Ubuntu repaired a signal-handling flaw in OpenSSH. The approved service procedure is to restore the vendor-fixed package binary, then restart the daemon so it loads that binary. [Ubuntu security notice](https://ubuntu.com/security/notices/USN-6859-1)

Use the work-order actions `restore-openssh-server` and `restart-ssh`. The maintenance runner operates a disposable, disconnected Ubuntu guest. It stages a harmless drill copy of the server, applies your order, verifies the installed and running executable and makes a new SSH login as an engineer. A version banner cannot pass the check. Run `~/check_goal_status.sh` for the first remaining milestone.
