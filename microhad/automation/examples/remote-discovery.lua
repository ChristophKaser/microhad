-- Replace Hall remote, then press buttons and inspect debug logs.
events.on({ type = "remote.action", device = "Hall remote" }, function(event)
  dump(event, "remote button")
end)
