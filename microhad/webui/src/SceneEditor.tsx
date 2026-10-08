import { useEffect, useState } from 'react'
import type { Device } from './api'
import { errorMessage, post, textRequest } from './api'
import { matterCall } from './matter'
import { isDimmable, newState, nextId, parseScenes, serializeScenes } from './scenes'
import type { SceneConfig, SceneState } from './scenes'

type Props = {
  devices: Device[]
  nativeStatuses: { ieee_address: string; group_id: number; status: string }[]
  mode: 'scenes' | 'remotes'
  onClose: () => void
  onSaved: () => void
}

const actionLabels: Record<string, string> = {
  on: 'Power on',
  off: 'Power off',
  arrow_left: '← Arrow left',
  arrow_right: '→ Arrow right',
  matter_press: 'Matter: press',
  matter_hold: 'Matter: hold',
  matter_release: 'Matter: release',
  matter_multi_press: 'Matter: multi-press',
}

function AccordionToggle({
  title,
  detail,
  id,
  open,
  onToggle,
}: {
  title: string
  detail: string
  id: string
  open: boolean
  onToggle: () => void
}) {
  return (
    <button
      className="accordion-toggle"
      type="button"
      aria-expanded={open}
      aria-controls={id}
      onClick={onToggle}
    >
      <span className="accordion-title">{title}</span>
      <span className="accordion-detail">{detail}</span>
      <span className="accordion-chevron" aria-hidden="true">⌄</span>
    </button>
  )
}

function SceneLightState({
  device,
  address,
  state,
  onChange,
}: {
  device: Device | undefined
  address: string
  state: SceneState
  onChange: (state: SceneState) => void
}) {
  const dimmable = device ? isDimmable(device) : false
  const setOn = (on: boolean) => {
    let brightness = state.brightness
    if (on && brightness < 0 && dimmable)
      brightness = device?.has_brightness ? device.brightness_percent : 100
    onChange({ ...state, on, brightness })
  }

  return (
    <div className="state-row">
      <span>{device?.name ?? address}</span>
      <label className="check">
        <input
          type="checkbox"
          checked={state.on}
          onChange={(event) => setOn(event.target.checked)}
        />{' '}
        On
      </label>
      {dimmable ? (
        <label className="field compact">
          Brightness %
          <input
            type="number"
            min="0"
            max="100"
            disabled={!state.on}
            value={state.brightness < 0 ? 100 : state.brightness}
            onChange={(event) => onChange({ ...state, brightness: Number(event.target.value) })}
          />
        </label>
      ) : (
        <span className="muted small">On/off light</span>
      )}
    </div>
  )
}

export function SceneEditor({ devices, nativeStatuses, mode, onClose, onSaved }: Props) {
  const [config, setConfig] = useState<SceneConfig | null>(null)
  const [error, setError] = useState('')
  const [loading, setLoading] = useState(true)
  const [saving, setSaving] = useState(false)
  const [openGroupId, setOpenGroupId] = useState<number | null>(null)
  const [openSceneKey, setOpenSceneKey] = useState<string | null>(null)
  const [openBindingIndex, setOpenBindingIndex] = useState<number | null>(null)
  const [matterTarget, setMatterTarget] = useState<Record<string, string>>({})
  const [matterGroup, setMatterGroup] = useState<Record<string, string>>({})
  const [matterMembers, setMatterMembers] = useState<Record<string, string[]>>({})
  const [matterBindStatus, setMatterBindStatus] = useState<Record<string, string>>({})
  useEffect(() => {
    let active = true
    textRequest('/api/scenes/config')
      .then((text) => {
        if (!active) return
        setConfig(parseScenes(text))
        setLoading(false)
      })
      .catch((e) => {
        if (!active) return
        setError(errorMessage(e))
        setLoading(false)
      })
    return () => {
      active = false
    }
  }, [])
  const change = (edit: (draft: SceneConfig) => void) =>
    setConfig((current) => {
      if (!current) return current
      const draft: SceneConfig = structuredClone(current)
      edit(draft)
      return draft
    })
  const lights = devices.filter((device) => device.has_light)
  const remotes = devices.filter((device) => device.has_remote)
  const remoteId = (device: Device) => device.source === 'matter' ? device.external_id : device.ieee_address
  const addGroup = () => {
    if (!config) return
    const id = nextId(config.groups, 0xfff7)
    if (!id) {
      setError('No group IDs remain')
      return
    }
    change((draft) => {
      draft.groups.push({ id, name: 'New group', members: [], scenes: [] })
    })
    setOpenGroupId(id)
    setOpenSceneKey(null)
  }
  const addScene = (groupIndex: number) => {
    if (!config) return
    const id = nextId(config.groups[groupIndex].scenes, 255)
    if (!id) {
      setError('No scene IDs remain')
      return
    }
    change((draft) => {
      const group = draft.groups[groupIndex]
      group.scenes.push({
        id,
        name: 'New scene',
        states: group.members.map((member) => newState(member, devices)),
      })
    })
    setOpenGroupId(config.groups[groupIndex].id)
    setOpenSceneKey(`${config.groups[groupIndex].id}/${id}`)
  }
  const removeGroup = (groupIndex: number) => {
    if (!config) return
    const groupId = config.groups[groupIndex].id
    change((draft) => {
      const [group] = draft.groups.splice(groupIndex, 1)
      draft.bindings = draft.bindings.filter((binding) => binding.groupId !== group.id)
      draft.nativeBindings = draft.nativeBindings.filter((binding) => binding.groupId !== group.id)
    })
    if (openGroupId === groupId) setOpenGroupId(null)
    if (openSceneKey?.startsWith(`${groupId}/`)) setOpenSceneKey(null)
    setOpenBindingIndex(null)
  }
  const setGroupMember = (groupIndex: number, address: string, selected: boolean) =>
    change((draft) => {
      const group = draft.groups[groupIndex]
      if (selected) {
        group.members.push(address)
        group.scenes.forEach((scene) => scene.states.push(newState(address, devices)))
      } else {
        group.members = group.members.filter((member) => member !== address)
        group.scenes.forEach((scene) => {
          scene.states = scene.states.filter((state) => state.ieee !== address)
        })
      }
    })
  const removeScene = (groupIndex: number, sceneIndex: number) => {
    if (!config) return
    const sceneKey = `${config.groups[groupIndex].id}/${config.groups[groupIndex].scenes[sceneIndex].id}`
    change((draft) => {
      const group = draft.groups[groupIndex]
      const [scene] = group.scenes.splice(sceneIndex, 1)
      draft.bindings.forEach((binding) => {
        if (binding.groupId === group.id)
          binding.sceneIds = binding.sceneIds.filter((id) => id !== scene.id)
      })
    })
    if (openSceneKey === sceneKey) setOpenSceneKey(null)
  }
  const setSceneState = (
    groupIndex: number,
    sceneIndex: number,
    address: string,
    state: SceneState
  ) =>
    change((draft) => {
      const scene = draft.groups[groupIndex].scenes[sceneIndex]
      const index = scene.states.findIndex((entry) => entry.ieee === address)
      if (index < 0) scene.states.push(state)
      else scene.states[index] = state
    })
  const save = async () => {
    if (!config) return
    try {
      const text = serializeScenes(config, devices)
      setError('')
      setSaving(true)
      await post('/api/scenes/config', text)
      onSaved()
      onClose()
    } catch (e) {
      setError(errorMessage(e))
    } finally {
      setSaving(false)
    }
  }
  return (
    <div
      className="modal"
      role="presentation"
      onMouseDown={(event) => {
        if (event.target === event.currentTarget) onClose()
      }}
    >
      <section
        className="dialog wide"
        role="dialog"
        aria-modal="true"
        aria-labelledby={`${mode}-editor-title`}
      >
        <div className="dialog-heading">
          <div>
            <p className="eyebrow">Zigbee automation</p>
            <h2 id={`${mode}-editor-title`}>{mode === 'scenes' ? 'Scenes & groups' : 'Remote controls'}</h2>
          </div>
          <button className="icon-button" onClick={onClose} aria-label="Close">
            ✕
          </button>
        </div>
        <p className="muted">{mode === 'scenes'
          ? 'Assign lights to groups and set scene states.'
          : 'Choose native light control or assign remote actions to gateway scenes.'}</p>
        {error && (
          <p className="error" role="alert">
            {error}
          </p>
        )}
        {loading && <p>Loading scene configuration…</p>}
        {config && (
          <>
            {mode === 'scenes' && config.groups.map((group, groupIndex) => (
              <section className="editor-card" key={group.id}>
                <AccordionToggle
                  title={group.name || 'Unnamed group'}
                  detail={`Zigbee group ${group.id} · ${group.members.length} lights · ${group.scenes.length} scenes`}
                  id={`group-panel-${group.id}`}
                  open={openGroupId === group.id}
                  onToggle={() => {
                    setOpenGroupId(openGroupId === group.id ? null : group.id)
                    setOpenSceneKey(null)
                  }}
                />
                <div
                  className="accordion-body"
                  id={`group-panel-${group.id}`}
                  hidden={openGroupId !== group.id}
                >
                  <div className="editor-row">
                    <label className="field grow">
                      Group name
                      <input
                        value={group.name}
                        onChange={(event) =>
                          change((d) => {
                            d.groups[groupIndex].name = event.target.value
                          })
                        }
                      />
                    </label>
                    <button
                      className="button danger"
                      onClick={() => removeGroup(groupIndex)}
                    >
                      Remove group
                    </button>
                  </div>
                  <h3>Lights</h3>
                  <div className="choice-grid">
                    {lights.map((light) => {
                      const selected = group.members.includes(light.ieee_address)
                      const assigned = config.groups.some(
                        (other) => other.id !== group.id && other.members.includes(light.ieee_address)
                      )
                      return (
                        <label className="check" key={light.ieee_address}>
                          <input
                            type="checkbox"
                            checked={selected}
                            disabled={assigned}
                            onChange={(event) =>
                              setGroupMember(groupIndex, light.ieee_address, event.target.checked)
                            }
                          />{' '}
                          {light.name}
                          {assigned ? ' (in another group)' : ''}
                        </label>
                      )
                    })}
                    {!lights.length && <span className="muted">No discovered lights available.</span>}
                  </div>
                  <h3>Scenes</h3>
                  {group.scenes.map((scene, sceneIndex) => {
                    const sceneKey = `${group.id}/${scene.id}`
                    return (
                      <section className="scene-card" key={scene.id}>
                        <AccordionToggle
                          title={scene.name || 'Unnamed scene'}
                          detail={`${group.members.length} lights`}
                          id={`scene-panel-${group.id}-${scene.id}`}
                          open={openSceneKey === sceneKey}
                          onToggle={() => setOpenSceneKey(openSceneKey === sceneKey ? null : sceneKey)}
                        />
                        <div
                          className="accordion-body"
                          id={`scene-panel-${group.id}-${scene.id}`}
                          hidden={openSceneKey !== sceneKey}
                        >
                          <div className="editor-row">
                            <label className="field grow">
                              Scene name
                              <input
                                value={scene.name}
                                onChange={(event) =>
                                  change((d) => {
                                    d.groups[groupIndex].scenes[sceneIndex].name = event.target.value
                                  })
                                }
                              />
                            </label>
                            <button
                              className="button danger"
                              onClick={() => removeScene(groupIndex, sceneIndex)}
                            >
                              Remove scene
                            </button>
                          </div>
                          {group.members.map((address) => {
                            const device = devices.find((item) => item.ieee_address === address)
                            const state =
                              scene.states.find((item) => item.ieee === address) ??
                              newState(address, devices)
                            return (
                              <SceneLightState
                                key={address}
                                device={device}
                                address={address}
                                state={state}
                                onChange={(next) => setSceneState(groupIndex, sceneIndex, address, next)}
                              />
                            )
                          })}
                          {!group.members.length && (
                            <p className="muted">Add a light to set its scene state.</p>
                          )}
                        </div>
                      </section>
                    )
                  })}
                  <button className="button" onClick={() => addScene(groupIndex)}>
                    ＋ Add scene
                  </button>
                </div>
              </section>
            ))}
            {mode === 'scenes' && <button className="button" onClick={addGroup}>
              ＋ Add group
            </button>}
            {mode === 'remotes' && <section className="editor-card">
              <h3>Remote bindings</h3>
              <p className="muted">Choose native control for direct On/Off and dimming while the gateway is off. Gateway scene rules stay saved and resume when you choose Gateway scenes.</p>
              {remotes.filter((remote) => remote.source !== 'matter').map((remote) => {
                const native = config.nativeBindings.find((binding) => binding.ieee === remote.ieee_address)
                const provisioned = nativeStatuses.find((item) => item.ieee_address === remote.ieee_address)
                return (
                  <div className="editor-row" key={remote.ieee_address}>
                    <span className="grow">{remote.name}</span>
                    <label className="field compact">
                      Control mode
                      <select
                        value={native ? 'native' : 'gateway'}
                        onChange={(event) => change((draft) => {
                          draft.nativeBindings = draft.nativeBindings.filter((item) => item.ieee !== remote.ieee_address)
                          const group = draft.groups.find((item) => item.id !== 901)
                          if (event.target.value === 'native' && group)
                            draft.nativeBindings.push({ ieee: remote.ieee_address, groupId: group.id })
                        })}
                      >
                        <option value="gateway">Gateway scenes</option>
                        <option value="native" disabled={!config.groups.some((group) => group.id !== 901)}>Native light control</option>
                      </select>
                    </label>
                    {native && (
                      <label className="field compact">
                        Light group
                        <select value={native.groupId} onChange={(event) => change((draft) => {
                          const entry = draft.nativeBindings.find((item) => item.ieee === remote.ieee_address)
                          if (entry) entry.groupId = Number(event.target.value)
                        })}>
                          {config.groups.filter((group) => group.id !== 901).map((group) =>
                            <option key={group.id} value={group.id}>{group.name}</option>
                          )}
                        </select>
                      </label>
                    )}
                    {native && <span className="muted small">{provisioned?.status ?? 'Save to provision'}</span>}
                  </div>
                )
              })}
              {remotes.filter((remote) => remote.source === 'matter').map((remote) => {
                const targets = devices.filter((device) => device.source === 'matter' && device.matter_node &&
                  device.matter_endpoint !== undefined && device.matter_servers?.includes(6))
                const members = devices.filter((device) => device.source === 'matter' && device.matter_node &&
                  device.matter_endpoint !== undefined && device.matter_servers?.includes(4) && device.matter_servers.includes(6))
                const selected = matterTarget[remote.id] ?? ''
                const selectedMembers = matterMembers[remote.id] ?? []
                return <div className="editor-row" key={remote.id}>
                  <span className="grow">{remote.name}</span>
                  <label className="field compact">Matter target
                    <select value={selected} onChange={(event) => setMatterTarget((current) => ({ ...current, [remote.id]: event.target.value }))}>
                      <option value="">Choose a Matter On/Off endpoint</option>
                      {targets.map((target) => <option key={target.id} value={`${target.matter_node}/${target.matter_endpoint}`}>{target.name}</option>)}
                    </select>
                  </label>
                  <button className="button" disabled={!selected} onClick={async () => {
                    const [node, endpointText] = selected.split('/')
                    const sourceNode = remote.matter_node
                    const sourceEndpoint = remote.matter_endpoint
                    if (!sourceNode || sourceEndpoint === undefined) return
                    try {
                      const controller = new AbortController()
                      const targetsToBind: { node: string; endpoint: number; cluster: number }[] = [
                        { node, endpoint: Number(endpointText), cluster: 6 },
                      ]
                      const selectedDevice = targets.find((item) => item.matter_node === node && item.matter_endpoint === Number(endpointText))
                      if (selectedDevice?.matter_servers?.includes(8)) targetsToBind.push({ node, endpoint: Number(endpointText), cluster: 8 })
                      await matterCall('/api/matter/devices/bind', { node: sourceNode, endpoint: sourceEndpoint, targets: targetsToBind }, controller.signal)
                      setMatterBindStatus((current) => ({ ...current, [remote.id]: 'Binding saved on remote' }))
                    } catch (cause) {
                      setMatterBindStatus((current) => ({ ...current, [remote.id]: errorMessage(cause) }))
                    }
                  }}>Bind directly</button>
                  {matterBindStatus[remote.id] && <span className="muted small">{matterBindStatus[remote.id]}</span>}
                  <label className="field compact">Matter group ID
                    <input type="number" min="1" max="65535" value={matterGroup[remote.id] ?? ''}
                      onChange={(event) => setMatterGroup((current) => ({ ...current, [remote.id]: event.target.value }))} />
                  </label>
                  <label className="field compact">Group members (select every endpoint in this Matter group)
                    <select multiple value={selectedMembers} onChange={(event) => setMatterMembers((current) => ({
                      ...current, [remote.id]: Array.from(event.target.selectedOptions, (option) => option.value),
                    }))}>
                      {members.map((member) => <option key={member.id} value={`${member.matter_node}/${member.matter_endpoint}`}>{member.name}</option>)}
                    </select>
                  </label>
                  <button className="button" disabled={!matterMembers[remote.id]?.length || !matterGroup[remote.id] || !/^\d+$/.test(matterGroup[remote.id]) ||
                    Number(matterGroup[remote.id]) < 1 || Number(matterGroup[remote.id]) > 65535}
                    onClick={async () => {
                      if (!remote.matter_node || remote.matter_endpoint === undefined) return
                      try {
                        const controller = new AbortController()
                        const groupMembers = selectedMembers.map((identity) => {
                          const [node, endpoint] = identity.split('/')
                          return { node, endpoint: Number(endpoint) }
                        })
                        await matterCall('/api/matter/devices/bind', {
                          node: remote.matter_node, endpoint: remote.matter_endpoint,
                          targets: [
                            { group: Number(matterGroup[remote.id]), members: groupMembers, cluster: 6 },
                            ...(groupMembers.length > 0 && groupMembers.every((member) => members.some((device) =>
                              device.matter_node === member.node && device.matter_endpoint === member.endpoint && device.matter_servers?.includes(8)))
                              ? [{ group: Number(matterGroup[remote.id]), members: groupMembers, cluster: 8 }] : []),
                          ],
                        }, controller.signal)
                        setMatterBindStatus((current) => ({ ...current, [remote.id]: 'Group keys, membership, and remote binding configured' }))
                      } catch (cause) {
                        setMatterBindStatus((current) => ({ ...current, [remote.id]: errorMessage(cause) }))
                      }
                    }}>Bind existing group</button>
                </div>
              })}
              {config.bindings.map((binding, index) => {
                const group = config.groups.find((item) => item.id === binding.groupId)
                const remote = remotes.find((item) => remoteId(item) === binding.ieee)
                return (
                  <section className="scene-card" key={index}>
                    <AccordionToggle
                      title={remote?.name ?? binding.ieee}
                      detail={`${actionLabels[binding.action] ?? binding.action} · ${group?.name ?? `Group ${binding.groupId}`}${config.nativeBindings.some((item) => item.ieee === binding.ieee) ? ' · paused in native mode' : ''}`}
                      id={`binding-panel-${index}`}
                      open={openBindingIndex === index}
                      onToggle={() => setOpenBindingIndex(openBindingIndex === index ? null : index)}
                    />
                    <div
                      className="accordion-body"
                      id={`binding-panel-${index}`}
                      hidden={openBindingIndex !== index}
                    >
                      <div className="binding-grid">
                        <label className="field">
                          Remote
                          <select
                            value={binding.ieee}
                            onChange={(event) =>
                              change((d) => {
                                d.bindings[index].ieee = event.target.value
                              })
                            }
                          >
                            {remotes.map((remote) => (
                              <option key={remoteId(remote)} value={remoteId(remote)}>
                                {remote.name}
                              </option>
                            ))}
                          </select>
                        </label>
                        <label className="field">
                          Action
                          <select
                            value={binding.action}
                            onChange={(event) =>
                              change((d) => {
                                d.bindings[index].action = event.target.value
                              })
                            }
                          >
                            {remote?.source === 'matter' ? <>
                              <option value="matter_press">Press</option>
                              <option value="matter_hold">Hold</option>
                              <option value="matter_release">Release</option>
                              <option value="matter_multi_press">Multi-press</option>
                            </> : <>
                              <option value="on">Power on</option>
                              <option value="off">Power off</option>
                              <option value="arrow_left">← Arrow left</option>
                              <option value="arrow_right">→ Arrow right</option>
                            </>}
                          </select>
                        </label>
                        <label className="field">
                          Group
                          <select
                            value={binding.groupId}
                            onChange={(event) =>
                              change((d) => {
                                d.bindings[index].groupId = Number(event.target.value)
                                d.bindings[index].sceneIds = []
                              })
                            }
                          >
                            {config.groups.map((item) => (
                              <option key={item.id} value={item.id}>
                                {item.name}
                              </option>
                            ))}
                          </select>
                        </label>
                        <button
                          className="button danger"
                          onClick={() => {
                            change((d) => {
                              d.bindings.splice(index, 1)
                            })
                            setOpenBindingIndex((current) =>
                              current === null || current === index ? null : current > index ? current - 1 : current
                            )
                          }}
                        >
                          Remove
                        </button>
                      </div>
                      <label className="check">
                        <input
                          type="checkbox"
                          checked={binding.off}
                          onChange={(event) =>
                            change((d) => {
                              d.bindings[index].off = event.target.checked
                            })
                          }
                        />{' '}
                        Turn group off
                      </label>
                      {!binding.off && (
                        <div className="choice-grid">
                          {group?.scenes.map((scene) => (
                            <label className="check" key={scene.id}>
                              <input
                                type="checkbox"
                                checked={binding.sceneIds.includes(scene.id)}
                                onChange={(event) =>
                                  change((d) => {
                                    const ids = d.bindings[index].sceneIds
                                    d.bindings[index].sceneIds = event.target.checked
                                      ? [...ids, scene.id]
                                      : ids.filter((id) => id !== scene.id)
                                  })
                                }
                              />{' '}
                              {scene.name}
                            </label>
                          ))}
                        </div>
                      )}
                    </div>
                  </section>
                )
              })}
              {!config.bindings.length && <p className="muted">No remote bindings configured.</p>}
              <button
                className="button"
                disabled={!remotes.length || !config.groups.length}
                onClick={() => {
                  setOpenBindingIndex(config.bindings.length)
                  change((d) => {
                    const group = d.groups[0]
                    d.bindings.push({
                      ieee: remotes[0].ieee_address,
                      action: 'on',
                      groupId: group.id,
                      off: false,
                      sceneIds: group.scenes.length ? [group.scenes[0].id] : [],
                    })
                  })
                }}
              >
                ＋ Add remote binding
              </button>
            </section>}
          </>
        )}
        <div className="dialog-actions">
          <button className="button" onClick={onClose}>
            Cancel
          </button>
          <button className="button primary" disabled={!config || saving} onClick={save}>
            {saving ? 'Saving…' : 'Save & sync scenes'}
          </button>
        </div>
      </section>
    </div>
  )
}
