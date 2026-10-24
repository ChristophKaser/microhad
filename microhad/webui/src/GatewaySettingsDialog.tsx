import { useEffect, useRef, useState } from 'react'
import { errorMessage, form, jsonRequest, post } from './api'

interface GatewaySettings {
  broadcast_passive_ack_quorum: number
  matter_country_code: string
  matter_regulatory_location: 'indoor' | 'outdoor'
  applied_value: number | null
  application_state: 'pending' | 'applied' | 'error' | 'unsupported'
  application_error: string
}
const countryNames = new Intl.DisplayNames(['en'], { type: 'region' })
const completeCountryCodes = 'AW AF AO AI AX AL AD AE AR AM AS AQ TF AG AU AT AZ BI BE BJ BQ BF BD BG BH BS BA BL BY BZ BM BO BR BB BN BT BV BW CF CA CC CH CL CN CI CM CD CG CK CO KM CV CR CU CW CX KY CY CZ DE DJ DM DK DO DZ EC EG ER EH ES EE ET FI FJ FK FR FO FM GA GB GE GG GH GI GN GP GM GW GQ GR GD GL GT GF GU GY HK HM HN HR HT HU ID IM IN IO IE IR IQ IS IL IT JM JE JO JP KZ KE KG KH KI KN KR KW LA LB LR LY LC LI LK LS LT LU LV MO MF MA MC MD MG MV MX MH MK ML MT MM ME MN MZ YT NA NC NE NF NG NI NU NL NO NP NR NZ OM PK PA PN PE PH PW PG PL PR KP PT PY PS PF QA RE RU RW SA SD SN SG GS SH SJ SB SL SV SM SO PM RS SS ST SR SK SI SE SZ SX SC SY TC TD TG TH TJ TK TM TL TO TT TN TR TV TW TZ UG UA UM UY US UZ VA VC VE VG VI VN VU WF WS YE ZA ZM ZW'
const countries = completeCountryCodes.split(' ').concat('MP MR MS MQ MU MW MY RO'.split(' '))
  .map(code => [code, countryNames.of(code) || code] as const)
  .filter((entry, index, values) => values.findIndex(candidate => candidate[0] === entry[0]) === index)
  .sort((left, right) => left[1].localeCompare(right[1]))

export function GatewaySettingsDialog({ onClose, onReplaceZigbeeNetwork }: { onClose: () => void; onReplaceZigbeeNetwork?: () => void }) {
  const [settings, setSettings] = useState<GatewaySettings | null>(null)
  const [value, setValue] = useState('')
  const [country, setCountry] = useState('DE')
  const [location, setLocation] = useState<'indoor' | 'outdoor'>('indoor')
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState('')
  const initialized = useRef(false)

  useEffect(() => {
    let active = true
    let timer: ReturnType<typeof setTimeout> | undefined
    async function refresh() {
      try {
        const data = await jsonRequest<GatewaySettings>('/api/settings')
        if (!active) return
        setSettings(data)
        if (!initialized.current) {
          setValue(String(data.broadcast_passive_ack_quorum))
          setCountry(data.matter_country_code)
          setLocation(data.matter_regulatory_location)
          initialized.current = true
        }
      } catch (reason) {
        if (active) setError(errorMessage(reason))
      }
      if (active) timer = setTimeout(() => void refresh(), 1000)
    }
    void refresh()
    return () => { active = false; if (timer) clearTimeout(timer) }
  }, [])

  async function save(event: React.FormEvent) {
    event.preventDefault()
    if (settings?.application_state !== 'unsupported' &&
        (!/^\d{1,3}$/.test(value) || Number(value) < 1 || Number(value) > 255)) {
      setError('Enter an integer between 1 and 255.')
      return
    }
    setSaving(true)
    setError('')
    try {
      const fields: Record<string, string | number> = {
        matter_country_code: country,
        matter_regulatory_location: location,
      }
      if (settings?.application_state !== 'unsupported') fields.broadcast_passive_ack_quorum = Number(value)
      setSettings(await post<GatewaySettings>('/api/settings', form(fields)))
    } catch (reason) {
      setError(errorMessage(reason))
    } finally {
      setSaving(false)
    }
  }

  const unsupported = settings?.application_state === 'unsupported'
  return <div className="modal">
    <form className="dialog" onSubmit={event => void save(event)} role="dialog" aria-modal="true" aria-labelledby="gateway-settings-title">
      <div className="dialog-heading">
        <h2 id="gateway-settings-title">Gateway settings</h2>
        <button type="button" className="icon-button" onClick={onClose} aria-label="Close">✕</button>
      </div>
      {error && <p className="error" role="alert">{error}</p>}
      <label className="field">
        <span>Broadcast confirmation threshold</span>
        <input type="number" min="1" max="255" step="1" required value={value}
          disabled={!settings || saving || unsupported} onChange={event => setValue(event.target.value)} />
      </label>
      <p className="muted">Number of eligible neighboring routers whose rebroadcasts confirm delivery.
        If fewer routers are eligible, all are required. Default: 8.</p>
      <p className="muted">Changes are saved and applied immediately. They are restored after a service restart.</p>
      <label className="field">
        <span>Matter commissioning country</span>
        <select value={country} disabled={!settings || saving} onChange={event => setCountry(event.target.value)}>
          {countries.map(([code, name]) => <option key={code} value={code}>{name} ({code})</option>)}
        </select>
      </label>
      <label className="field">
        <span>Thread radio location</span>
        <select value={location} disabled={!settings || saving} onChange={event => setLocation(event.target.value as 'indoor' | 'outdoor')}>
          <option value="indoor">Indoor</option><option value="outdoor">Outdoor</option>
        </select>
      </label>
      <p className="muted">The country and installation location are sent to Matter accessories during commissioning. Default: Germany, indoor.</p>
      <div role="status" aria-live="polite">
        {!settings ? <p>Loading settings…</p> : <>
          {settings.application_state === 'pending' && <p>Saved; awaiting Ziggurat. If disconnected, this will apply after reconnect.</p>}
          {settings.application_state === 'error' && <p className="error">{settings.application_error}</p>}
          {unsupported && <p>This setting is unavailable with the current coordinator.</p>}
        </>}
      </div>
      <div className="dialog-actions">
        {onReplaceZigbeeNetwork && <button type="button" className="button danger" disabled={saving} onClick={onReplaceZigbeeNetwork}>Replace Zigbee network</button>}
        <button type="button" className="button" onClick={onClose}>Cancel</button>
        <button type="submit" className="button primary" disabled={!settings || saving}>{saving ? 'Saving…' : 'Save settings'}</button>
      </div>
    </form>
  </div>
}
