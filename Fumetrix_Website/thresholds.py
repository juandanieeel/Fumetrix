"""Display thresholds shared with the device protocol."""

FIELDS = (
    ('temp_min', 'Temperature min (°C)', -45, 130, 26),
    ('temp_max', 'Temperature max (°C)', -45, 130, 32),
    ('rh_min', 'Humidity min (% RH)', 0, 100, 70),
    ('rh_max', 'Humidity max (% RH)', 0, 100, 85),
    ('pm25_max', 'PM2.5 max (µg/m³)', 0, 65535, 35),
    ('voc_max', 'VOC index max', 1, 500, 120),
    ('nox_max', 'NOx index max', 1, 500, 10),
)


def validate(values):
    if not isinstance(values, dict) or set(values) != {field[0] for field in FIELDS}:
        raise ValueError('Enter all seven limits.')
    for key, label, minimum, maximum, _ in FIELDS:
        value = values[key]
        if key.startswith('temp_'):
            if (type(value) not in (int, float) or not minimum <= value <= maximum
                    or value != round(value, 1)):
                raise ValueError(f'{label} must be between {minimum} and {maximum}, with at most one decimal place.')
        elif type(value) is not int or not minimum <= value <= maximum:
            raise ValueError(f'{label} must be a whole number from {minimum} to {maximum}.')
    if values['temp_min'] > values['temp_max'] or values['rh_min'] > values['rh_max']:
        raise ValueError('Each minimum must be at or below its maximum.')
    return values


def encode(values):
    return ','.join(format(values[key], 'g') if key.startswith('temp_') else str(values[key]) for key, *_ in FIELDS)


def load_limits(db):
    row = db.execute('SELECT limits FROM oled_thresholds WHERE id = 1').fetchone()
    return row[0] if row else None


def decode(packet):
    values = {}
    for field, value in zip(FIELDS, packet.split(','), strict=True):
        key = field[0]
        values[key] = float(value) if key.startswith('temp_') else int(value)
    return validate(values)
