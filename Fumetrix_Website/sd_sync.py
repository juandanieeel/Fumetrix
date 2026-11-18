"""Read the journal in order. SQLite's unique card/slot key makes retries safe."""

SD_MODES = {0: 'AUTO', 1: 'MAINTENANCE', 2: 'MANUAL'}


def sync_sd(bridge, status, now, received_at):
    card, count = map(int, status['sd_log'].split(','))
    if not 0 < card <= 0xFFFFFFFF or not 0 <= count <= 4096:
        return
    if bridge.cloud and bridge.last_synced_sd is not None:
        synced_card, synced_count, recheck_at = bridge.last_synced_sd
        if (card, count) == (synced_card, synced_count) and now < recheck_at:
            return
    try:
        values = list(map(int, (status['sd_row'] or '').split(',')))
    except ValueError:
        values = []
    with bridge.connect() as db:
        db.execute('BEGIN IMMEDIATE')
        slot = db.execute(
            '''SELECT MAX(
                COALESCE((SELECT MAX(sd_slot) + 1 FROM readings WHERE sd_card = ?), 0),
                COALESCE((SELECT next_slot FROM sd_cleared WHERE sd_card = ?), 0))''',
            (card, card),
        ).fetchone()[0]
        if slot < count and len(values) == 12:
            c, i, uptime, temp, rh, pm1, pm25, pm10, voc, nox, mode, duty = values
            if (c == card and i == slot and 0 <= uptime <= 4294967
                    and (temp == -32768 or -450 <= temp <= 1300)
                    and (rh == -32768 or 0 <= rh <= 1000)
                    and all(-1 <= pm <= 65535 for pm in (pm1, pm25, pm10))
                    and 0 <= voc <= 500 and 0 <= nox <= 500
                    and mode in SD_MODES and (duty == 0 or 20 <= duty <= 100)):
                db.execute(
                    '''INSERT OR IGNORE INTO readings
                       (recorded_at, mode, temperature, humidity, pm1, pm25, pm10,
                        voc_index, nox_index, sd_card, sd_slot, uptime_s, fan_duty)
                       VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)''',
                    (received_at, SD_MODES[mode],
                     None if temp == -32768 else temp / 10,
                     None if rh == -32768 else rh / 10,
                     None if pm1 < 0 else pm1, None if pm25 < 0 else pm25,
                     None if pm10 < 0 else pm10, voc or None, nox or None,
                     card, slot, uptime, duty))
                slot += 1
    # Advance only after the transaction commits; the SD copy stays intact.
    if bridge.cloud and slot >= count:
        bridge.last_synced_sd = card, count, now + 30
    if slot < count and now >= bridge.next_sd_request:
        bridge.next_sd_request = now + 1
        try:
            bridge.transport('/sd-read?slot=' + str(slot), 'POST')
        except (OSError, ValueError):
            pass
