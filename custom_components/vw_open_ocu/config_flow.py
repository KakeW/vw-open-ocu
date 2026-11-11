"""Configure a gateway by its broker topic identifier."""

import voluptuous as vol

from homeassistant import config_entries

from . import DOMAIN, GATEWAY_ID


class ConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_user(self, user_input=None):
        errors = {}
        if user_input is not None:
            gateway_id = user_input["gateway_id"].strip()
            if GATEWAY_ID.fullmatch(gateway_id):
                await self.async_set_unique_id(gateway_id)
                self._abort_if_unique_id_configured()
                return self.async_create_entry(title=f"VW Open OCU ({gateway_id})",
                                               data={"gateway_id": gateway_id})
            errors["gateway_id"] = "invalid_gateway_id"
        return self.async_show_form(
            step_id="user", data_schema=vol.Schema({vol.Required("gateway_id"): str}),
            errors=errors,
        )
