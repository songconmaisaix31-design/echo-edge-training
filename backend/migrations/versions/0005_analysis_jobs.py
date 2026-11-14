"""Persist Stanford analysis jobs without altering existing records."""
from alembic import op
import sqlalchemy as sa

revision = "0005"
down_revision = "0004"


def upgrade():
    op.create_table(
        "analysis_jobs",
        sa.Column("id", sa.Integer(), primary_key=True),
        sa.Column("exam_id", sa.Integer(), sa.ForeignKey("examinations.id"), nullable=False),
        sa.Column("media_id", sa.String(32), sa.ForeignKey("media.id"), nullable=False),
        sa.Column("model_type", sa.String(40), nullable=False),
        sa.Column("model_version", sa.String(80), nullable=False),
        sa.Column("input_sha256", sa.String(64), nullable=False),
        sa.Column("state", sa.String(16), nullable=False),
        sa.Column("error", sa.Text()),
        sa.Column("output", sa.Text()),
        sa.Column("created", sa.DateTime(), nullable=False),
        sa.Column("completed", sa.DateTime()),
        sa.Column("actor", sa.Integer(), sa.ForeignKey("users.id"), nullable=False),
    )
    op.create_index("ix_analysis_jobs_exam_id", "analysis_jobs", ["exam_id"])
    op.create_index("ix_analysis_jobs_media_id", "analysis_jobs", ["media_id"])


def downgrade():
    op.drop_table("analysis_jobs")
