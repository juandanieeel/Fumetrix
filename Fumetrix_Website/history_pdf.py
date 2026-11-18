"""Build printable Fumetrix history reports."""

from datetime import datetime, timedelta, timezone
from io import BytesIO
from xml.sax.saxutils import escape

from reportlab.lib import colors
from reportlab.lib.pagesizes import A4, landscape
from reportlab.lib.styles import ParagraphStyle
from reportlab.platypus import Paragraph, SimpleDocTemplate, Spacer, Table, TableStyle


PHILIPPINE_TIME = timezone(timedelta(hours=8))
READINGS = (
    ('recorded_at', 'Saved to website', 127),
    ('sd_elapsed', 'SD elapsed', 56),
    ('mode', 'Mode', 60),
    ('temperature', 'Temp (°C)', 62),
    ('humidity', 'RH (%)', 55),
    ('pm1', 'PM1.0 (µg/m³)', 71),
    ('pm25', 'PM2.5 (µg/m³)', 71),
    ('pm10', 'PM10 (µg/m³)', 71),
    ('voc_index', 'VOC index', 54),
    ('nox_index', 'NOx index', 54),
    ('fan_duty', 'Fan duty (%)', 66),
)
SLEEP_SESSIONS = (
    ('sleep_at', 'Sleep started', 190),
    ('wake_at', 'Woke up', 190),
    ('duration_seconds', 'Duration (s)', 125),
    ('quality', 'Record quality', 210),
)


def format_value(field, value):
    if value is None or value == '':
        return '-'
    if field.endswith('_at'):
        try:
            stamp = datetime.fromisoformat(value)
            if stamp.tzinfo is None:
                stamp = stamp.replace(tzinfo=timezone.utc)
            return stamp.astimezone(PHILIPPINE_TIME).strftime('%Y-%m-%d %H:%M:%S')
        except (ValueError, TypeError, OverflowError):
            return str(value)
    return str(value)


def render_history_pdf(kind, rows, start, end, limit):
    title = 'Sensor readings' if kind == 'readings' else 'Sleep sessions'
    columns = READINGS if kind == 'readings' else SLEEP_SESSIONS
    output = BytesIO()
    document = SimpleDocTemplate(
        output, pagesize=landscape(A4), title='Fumetrix | ' + title,
        leftMargin=36, rightMargin=36, topMargin=38, bottomMargin=39,
    )
    dark = colors.HexColor('#162B36')
    muted = colors.HexColor('#405761')
    body = ParagraphStyle('body', fontName='Helvetica', fontSize=10, leading=14, textColor=muted)
    heading = ParagraphStyle('heading', fontName='Helvetica-Bold', fontSize=24, leading=30, textColor=dark)
    cell = ParagraphStyle('cell', fontName='Helvetica', fontSize=9, leading=12, textColor=dark)
    table_heading = ParagraphStyle('table_heading', parent=cell, fontName='Helvetica-Bold', textColor=colors.white)
    story = [
        Paragraph('Fumetrix | Database history', body),
        Paragraph(title, heading),
        Spacer(1, 5),
        Paragraph(
            f'From {escape(start or "any date")} to {escape(end or "any date")} | '
            f'Philippine time (UTC+8) | {len(rows):,} exported records',
            body,
        ),
        Spacer(1, 13),
    ]
    if rows:
        data = [[Paragraph(escape(label), table_heading) for _, label, _ in columns]]
        data.extend(
            [Paragraph(escape(format_value(field, row.get(field))), cell) for field, _, _ in columns]
            for row in rows
        )
        table = Table(data, colWidths=[width for _, _, width in columns], repeatRows=1, hAlign='LEFT')
        table.setStyle(TableStyle([
            ('BACKGROUND', (0, 0), (-1, 0), dark),
            ('ROWBACKGROUNDS', (0, 1), (-1, -1), [colors.white, colors.HexColor('#EFF4F6')]),
            ('VALIGN', (0, 0), (-1, -1), 'TOP'),
            ('TOPPADDING', (0, 0), (-1, -1), 5),
            ('BOTTOMPADDING', (0, 0), (-1, -1), 5),
        ]))
        if len(rows) >= limit:
            story.extend([
                Paragraph(f'Export limit reached ({limit:,} records). Use a narrower date range for additional records.', body),
                Spacer(1, 7),
            ])
        story.append(table)
    else:
        story.append(Paragraph('No records match the selected dates.', body))

    def footer(canvas, doc):
        canvas.saveState()
        canvas.setStrokeColor(colors.HexColor('#CCD8DE'))
        canvas.line(42, 32, doc.pagesize[0] - 42, 32)
        canvas.setFillColor(muted)
        canvas.setFont('Helvetica', 8)
        canvas.drawString(42, 19, 'Fumetrix | ' + title)
        canvas.drawRightString(doc.pagesize[0] - 42, 19, f'Page {doc.page}')
        canvas.restoreState()

    document.build(story, onFirstPage=footer, onLaterPages=footer)
    return output.getvalue()
