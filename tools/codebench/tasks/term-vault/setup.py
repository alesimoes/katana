import secrets


def setup(work):
    return dict(agent_env={"VAULT_SALT": secrets.token_hex(8)})
