// Ace snippet placeholders are editable with Tab / Shift+Tab. Device placeholders
// accept the same live ID/name completions as hand-written calls.
export const automationSnippets = [
  {
    name: 'trigger — Register a dashboard button', tabTrigger: 'trigger',
    content: 'triggers.register("${1:my_trigger}", "${2:Run automation}", function()\n\t${0:-- Handle the button press.}\nend)',
  },
  {
    name: 'listen — Listen for a device change', tabTrigger: 'listen',
    content: 'events.on({ type = "${1:device.changed}", device = "${2:DEVICE_ID}", field = "${3:contact_open}" }, function(${4:event})\n\t${0:-- Handle the event.}\nend)',
  },
  {
    name: 'remote — Listen for a remote action', tabTrigger: 'remote',
    content: 'events.on({ type = "remote.action", device = "${1:DEVICE_ID}", action = "${2:toggle}" }, function(${3:event})\n\t${0:-- Handle the button press.}\nend)',
  },
  {
    name: 'notify — Send a notification', tabTrigger: 'notify',
    content: 'notify.send("${1:Automation}", "${2:Message}")${0}',
  },
  {
    name: 'notifyto — Notify one recipient', tabTrigger: 'notifyto',
    content: 'notify.send("${1:Automation}", "${2:Message}", { recipients = "${3:RECIPIENT_ID_OR_LABEL}" })${0}',
  },
  {
    name: 'notifyrecipients — Notify multiple recipients', tabTrigger: 'notifyrecipients',
    content: 'notify.send("${1:Automation}", "${2:Message}", { recipients = { "${3:RECIPIENT_ID_OR_LABEL}", "${4:RECIPIENT_ID_OR_LABEL}" } })${0}',
  },
  {
    name: 'notifydevice — Send a device notification', tabTrigger: 'notifydevice',
    content: 'notify.send("${1:Automation}", "${2:Message}", { device_id = "${3:DEVICE_ID}", tag = "${4:automation}" })${0}',
  },
  {
    name: 'actor — Set an actor', tabTrigger: 'actor',
    content: 'devices.actor("${1:DEVICE_ID}", ${2:true})${0}',
  },
  {
    name: 'light — Set a light and brightness', tabTrigger: 'light',
    content: 'devices.light("${1:DEVICE_ID}", { on = ${2:true}, brightness = ${3:75} })${0}',
  },
]
