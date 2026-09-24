import { useEffect, useState } from 'react'
import type { Device } from './api'
import { errorMessage, post, textRequest } from './api'
import { isDimmable, newState, nextId, parseScenes, serializeScenes } from './scenes'
import type { SceneConfig, SceneState } from './scenes'

type Props = { devices: Device[]; onClose: () => void; onSaved: () => void }

export function SceneEditor({ devices, onClose, onSaved }: Props) {
  const [config, setConfig] = useState<SceneConfig | null>(null)
  const [error, setError] = useState('')
  const [loading, setLoading] = useState(true)
  const [saving, setSaving] = useState(false)
  useEffect(() => {
    textRequest('/api/scenes/config')
      .then((text) => {
        setConfig(parseScenes(text))
        setLoading(false)
      })
      .catch((e) => {
        setError(errorMessage(e))
        setLoading(false)
      })
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
  }
  const removeGroup = (groupIndex: number) =>
    change((draft) => {
      const [group] = draft.groups.splice(groupIndex, 1)
      draft.bindings = draft.bindings.filter((binding) => binding.groupId !== group.id)
    })
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
  const removeScene = (groupIndex: number, sceneIndex: number) =>
    change((draft) => {
      const group = draft.groups[groupIndex]
      const [scene] = group.scenes.splice(sceneIndex, 1)
      draft.bindings.forEach((binding) => {
        if (binding.groupId === group.id)
          binding.sceneIds = binding.sceneIds.filter((id) => id !== scene.id)
      })
    })
  const setSceneState = (
    groupIndex: number,
    sceneIndex: number,
    address: string,
    update: (state: SceneState) => void
  ) =>
    change((draft) => {
      const scene = draft.groups[groupIndex].scenes[sceneIndex]
      let state = scene.states.find((entry) => entry.ieee === address)
      if (!state) {
        state = newState(address, devices)
        scene.states.push(state)
      }
      update(state)
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
        aria-labelledby="scene-editor-title"
      >
        <div className="dialog-heading">
          <div>
            <p className="eyebrow">Zigbee automation</p>
            <h2 id="scene-editor-title">Scenes & groups</h2>
          </div>
          <button className="icon-button" onClick={onClose} aria-label="Close">
            ✕
          </button>
        </div>
        <p className="muted">Assign lights to groups, set scene states, and bind remote actions.</p>
        {error && (
          <p className="error" role="alert">
            {error}
          </p>
        )}
        {loading && <p>Loading scene configuration…</p>}
        {config && (
          <>
            {config.groups.map((group, groupIndex) => (
              <section className="editor-card" key={group.id}>
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
                <p className="muted small">Zigbee group {group.id}</p>
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
                {group.scenes.map((scene, sceneIndex) => (
                  <div className="scene-card" key={scene.id}>
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
                      const dimmable = device ? isDimmable(device) : false
                      const updateState = (field: 'on' | 'brightness', value: boolean | number) =>
                        setSceneState(groupIndex, sceneIndex, address, (item) => {
                          if (field === 'on') {
                            item.on = Boolean(value)
                            if (item.on && item.brightness < 0 && device && dimmable)
                              item.brightness = device.has_brightness
                                ? device.brightness_percent
                                : 100
                          } else item.brightness = Number(value)
                        })
                      return (
                        <div className="state-row" key={address}>
                          <span>{device?.name ?? address}</span>
                          <label className="check">
                            <input
                              type="checkbox"
                              checked={state.on}
                              onChange={(event) => updateState('on', event.target.checked)}
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
                                onChange={(event) =>
                                  updateState('brightness', Number(event.target.value))
                                }
                              />
                            </label>
                          ) : (
                            <span className="muted small">On/off light</span>
                          )}
                        </div>
                      )
                    })}
                    {!group.members.length && (
                      <p className="muted">Add a light to set its scene state.</p>
                    )}
                  </div>
                ))}
                <button className="button" onClick={() => addScene(groupIndex)}>
                  ＋ Add scene
                </button>
              </section>
            ))}
            <button className="button" onClick={addGroup}>
              ＋ Add group
            </button>
            <section className="editor-card">
              <h3>Remote bindings</h3>
              {config.bindings.map((binding, index) => {
                const group = config.groups.find((item) => item.id === binding.groupId)
                return (
                  <div className="scene-card" key={index}>
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
                            <option key={remote.ieee_address} value={remote.ieee_address}>
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
                          <option value="on">Power on</option>
                          <option value="off">Power off</option>
                          <option value="arrow_left">← Arrow left</option>
                          <option value="arrow_right">→ Arrow right</option>
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
                        onClick={() =>
                          change((d) => {
                            d.bindings.splice(index, 1)
                          })
                        }
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
                )
              })}
              {!config.bindings.length && <p className="muted">No remote bindings configured.</p>}
              <button
                className="button"
                disabled={!remotes.length || !config.groups.length}
                onClick={() =>
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
                }
              >
                ＋ Add remote binding
              </button>
            </section>
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
