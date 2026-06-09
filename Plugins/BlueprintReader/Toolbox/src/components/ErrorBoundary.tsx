import { Component, type ErrorInfo, type ReactNode } from 'react';

// TBX-P1: a render throw anywhere in the tree used to white-screen the whole app
// (no recovery, no message). This boundary catches it and shows the error + a
// Reload, so one page's bug doesn't brick the Toolbox.
interface Props { children: ReactNode }
interface State { error: Error | null }

export default class ErrorBoundary extends Component<Props, State> {
  state: State = { error: null };

  static getDerivedStateFromError(error: Error): State {
    return { error };
  }

  componentDidCatch(error: Error, info: ErrorInfo) {
    // Surfaced to the devtools/console; the packaged build has source maps (P6).
    console.error('Toolbox render error:', error, info.componentStack);
  }

  render() {
    if (this.state.error) {
      return (
        <div className="p-8 max-w-2xl mx-auto text-sm" role="alert">
          <h1 className="text-lg font-semibold text-red-400 mb-2">Something went wrong</h1>
          <p className="text-gray-400 mb-3">
            A page failed to render. This is a bug in the Toolbox — the rest of the app is unaffected.
          </p>
          <pre className="bg-black/40 border border-ue-border rounded p-3 text-xs text-red-300 whitespace-pre-wrap break-all mb-4">
            {this.state.error.message}
          </pre>
          <button
            onClick={() => this.setState({ error: null })}
            className="px-4 py-2 bg-ue-accent hover:bg-ue-accent-hover rounded text-sm font-medium text-white"
          >
            Dismiss &amp; retry
          </button>
        </div>
      );
    }
    return this.props.children;
  }
}
